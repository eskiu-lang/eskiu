// type_checker_generic.cpp: value cycles, expression types and generic instances (split from type_checker.cpp).
#include "type_checker.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <climits>
#include <set>
#include <functional>

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with codegen; see template_utils.h.
#include "../template_utils.h"
#include "../ast/type_qual.h"
#include "../ast/ast_walk.h"

// A struct (or union) that contains itself by value, directly or through other by-value
// fields, has no finite layout. Pointer and slice fields break the cycle; a fixed-size
// array of a struct does not. A sum type holds each variant's payload by value, so an
// enum whose payload holds the enum (`enum L { Cons(int, L), Nil }`) is the same error.
void TypeChecker::checkValueCycles(Program* program) {
    // Each aggregate's by-value members: its fields, or an enum's variant payloads.
    struct VField { std::string type, desc; };
    struct VNode { Decl* decl; const char* kind; std::vector<std::string> tparams; std::vector<VField> fields; };
    std::map<std::string, VNode> all;
    for (const auto& decl : program->declarations) {
        if (auto* sd = nodeCast<StructDecl>(decl.get())) {
            VNode n{sd, "struct", sd->typeParams, {}};
            for (const auto& f : sd->fields) n.fields.push_back({f.type, "field '" + f.name + "'"});
            all[sd->name] = n;
        } else if (auto* ud = nodeCast<UnionDecl>(decl.get())) {
            VNode n{ud, "struct", {}, {}};
            for (const auto& f : ud->fields) n.fields.push_back({f.type, "field '" + f.name + "'"});
            all[ud->name] = n;
        } else if (auto* ed = nodeCast<EnumDecl>(decl.get()); ed && ed->isADT()) {
            VNode n{ed, "enum", ed->typeParams, {}};
            for (size_t i = 0; i < ed->members.size() && i < ed->payloads.size(); ++i)
                for (const auto& pt : ed->payloads[i]) n.fields.push_back({pt, "variant '" + ed->members[i].first + "'"});
            all[ed->name] = n;
        }
    }
    std::map<std::string, VNode*> decls;   // the non-generic ones
    for (auto& kv : all) if (kv.second.tparams.empty()) decls[kv.first] = &kv.second;
    auto byValueStruct = [&](const std::string& ft) -> std::string {
        // Peel array dimensions first: `S[2]` holds S by value, but normalizeType only
        // resolves a bare struct name.
        // An alias may name an array (`type AR = R[2]`), so peel again after resolving it.
        ty::Type t = ty::Type::parse(tyq::strip(ft));
        for (int hops = 0; hops < 32; ++hops) {
            while (t.kind == ty::Type::Kind::Array && t.elem) { ty::Type e = *t.elem; t = e; }
            t = ty::Type::parse(normalizeType(t.str()));
            if (t.kind != ty::Type::Kind::Array) break;
        }
        if (t.kind != ty::Type::Kind::Struct && !(t.kind == ty::Type::Kind::Named && adtEnums.count(t.name))) return "";
        std::string n = t.nominalName();
        return decls.count(n) ? n : "";
    };
    std::map<std::string, int> state;   // 0 = unvisited, 1 = on stack, 2 = done
    std::set<std::string> reported;
    std::function<void(const std::string&)> dfs = [&](const std::string& n) {
        state[n] = 1;
        for (const auto& f : decls[n]->fields) {
            std::string m = byValueStruct(f.type);
            if (m.empty()) continue;
            if (state[m] == 1) {
                if (reported.insert(m).second)
                    errorAtDecl(decls[m]->decl, std::string(decls[m]->kind) + " '" + m + "' contains itself by value (through " +
                                      f.desc + " of '" + n + "'); use a pointer");
            } else if (state[m] == 0) dfs(m);
        }
        state[n] = 2;
    };
    for (const auto& kv : decls) if (state[kv.first] == 0) dfs(kv.first);

    // Through generic instances: walk the by-value field spellings from every struct (a
    // generic one with its own parameters, `N<T>`), substituting each instance's type
    // arguments. A spelling that recurs is a cycle (`N<T> next`, or `S` holding a
    // `W<S>` that holds its `T` by value); instances that only ever grow (`N<N<T>> next`)
    // have no finite layout either. A cycle of plain structs is reported above.
    auto baseOf = [](const std::string& sp) { return sp.substr(0, sp.find('<')); };
    // The struct spelling a field of type `ft` holds by value ("" if none).
    // An alias is resolved (`type RA = W<RB>` holds a `W<RB>`), then arrays peeled again.
    auto valueCore = [&](const std::string& ft) -> std::string {
        ty::Type t = ty::Type::parse(tyq::strip(ft));
        for (int hops = 0; hops < 32; ++hops) {
            while ((t.kind == ty::Type::Kind::Array) && t.elem) { ty::Type e = *t.elem; t = e; }
            auto al = t.kind == ty::Type::Kind::Named ? typeAliases.find(t.name) : typeAliases.end();
            if (al == typeAliases.end()) break;
            t = ty::Type::parse(tyq::strip(al->second));
        }
        if (t.kind != ty::Type::Kind::Template && t.kind != ty::Type::Kind::Named) return "";
        return all.count(t.name) ? t.str() : "";
    };
    const size_t kMaxNest = 64;
    std::set<std::string> reportedGen;
    std::vector<std::string> stack;
    std::function<bool(const std::string&, Decl*)> walk = [&](const std::string& sp, Decl* root) -> bool {
        VNode& d = all[baseOf(sp)];
        std::map<std::string, std::string> subs;
        if (!d.tparams.empty()) {
            auto args = splitTemplateType(sp).second;
            if (args.size() != d.tparams.size()) return false;
            for (size_t i = 0; i < args.size(); ++i) subs[d.tparams[i]] = args[i];
        }
        stack.push_back(sp);
        for (const auto& f : d.fields) {
            std::string core = valueCore(subs.empty() ? f.type : substType(f.type, subs));
            if (core.empty()) continue;
            auto hit = std::find(stack.begin(), stack.end(), core);
            std::string msg, key;
            if (hit != stack.end()) {
                if (std::none_of(hit, stack.end(), [](const std::string& s) { return s.find('<') != std::string::npos; }))
                    continue;
                key = baseOf(core);
                msg = std::string(all[key].kind) + " '" + core + "' contains itself by value (through " + f.desc +
                      " of '" + sp + "'); use a pointer";
            } else if (stack.size() >= kMaxNest) {
                key = baseOf(stack.front());
                msg = std::string(all[key].kind) + " '" + key + "' contains itself by value through ever-deeper generic instances "
                      "(" + f.desc + " of '" + baseOf(sp) + "'); use a pointer";
            } else {
                if (walk(core, root)) { stack.pop_back(); return true; }
                continue;
            }
            if (reportedGen.insert(key).second) errorAt(root, msg);
            stack.pop_back();
            return true;
        }
        stack.pop_back();
        return false;
    };
    for (const auto& decl : program->declarations) {
        auto it = all.find(decl->name);
        if (it == all.end() || it->second.decl != decl.get()) continue;
        std::string sp = decl->name;
        const auto& tp = it->second.tparams;
        if (!tp.empty()) {
            sp += "<";
            for (size_t i = 0; i < tp.size(); ++i) sp += (i ? "," : "") + tp[i];
            sp += ">";
        }
        stack.clear();
        walk(ty::Type::parse(sp).str(), decl.get());
    }
}

std::string TypeChecker::getExpressionType(Expr* expr) {
    auto it = expressionTypes.find(expr);
    if (it != expressionTypes.end()) {
        // A type built from source spellings inside a generic instance (a lambda's
        // `fn(T)->T`) still names the parameter: resolve it like any declared type.
        const std::string& t = inInstance ? resolveInstType(it->second) : it->second;
        // Never spelled as a type alias: every shape check (pointer, array, slice, fn
        // value, interface, struct) sees the alias target. A const-qualified spelling is
        // kept whole (the const checks read it).
        if (typeAliases.empty() || tyq::baseConst(t)) return t;
        return dealiasOperand(t);
    }
    return "unknown";
}

std::string TypeChecker::resolveInstType(const std::string& t) const {
    if (!inInstance || instSubs.empty()) return t;
    for (const auto& kv : instSubs)
        if (t.find(kv.first) != std::string::npos) return substType(t, instSubs);
    return t;
}

void TypeChecker::queueInstance(FunctionDecl* fn, const std::vector<std::string>& typeParams,
                                const std::map<std::string, std::string>& subs,
                                const std::string& name, const std::string& suffix,
                                const std::string& mangled, const std::string& selfType,
                                const std::string& file) {
    if (!fn || !fn->body) return;
    std::string display = name + "<";
    for (size_t i = 0; i < typeParams.size(); ++i) {
        auto it = subs.find(typeParams[i]);
        std::string a = it == subs.end() ? typeParams[i] : it->second;
        for (size_t p; (p = a.find("struct:")) != std::string::npos; ) a.erase(p, 7);
        display += (i ? "," : "") + a;
    }
    display += ">" + suffix;
    if (!queuedInstances.insert(display).second) return;
    pendingInstances.push_back({fn, subs, display, mangled, selfType,
                                file.empty() ? diagFile() : file, instDepth + 1});
}

// Check each queued generic instance: the template body, with its type parameters
// bound to the instance's arguments. Checking one may queue more (nested generic
// calls, generic structs named in the body), so this runs until the queue drains.
std::string TypeChecker::genericCallRet(FunctionDecl* fd, const std::map<std::string, std::string>& subs) {
    std::string r = substType(fd->returnType, subs);
    if (fd->isAsync) r = "*Future<" + (r == "void" ? std::string("uint8") : r) + ">";
    return normalizeType(r);
}

// Decide the env of every lambda whose escape depended on generic instances. Set both ways:
// the check after the async lowering decides again.
void TypeChecker::finalizeGenericEscapes() {
    std::set<EscKey> escaping;
    for (const auto& [k, e] : genericParamEscapes)
        if (e) escaping.insert(k);
    auto escapes = [&](const EscKey& k) { return escaping.count(k) || !genericParamEscapes.count(k); };
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& [k, to] : genericParamForwards) {
            if (escaping.count(k)) continue;
            for (const auto& d : to)
                if (escapes(d)) { escaping.insert(k); changed = true; break; }
        }
    }
    for (auto& [lam, d] : deferredLambdas) {
        bool e = d.escapes;
        for (const auto& k : d.via) e = e || escapes(k);
        lam->escapes = e;
    }
}

void TypeChecker::checkPendingInstances() {
    static const int kMaxDepth = 64;
    for (size_t i = 0; i < pendingInstances.size(); ++i) {
        PendingInstance p = pendingInstances[i];
        if (p.depth > kMaxDepth) {
            curFile = p.file;
            errorAt(p.fn, "generic instantiation of '" + p.display + "' is nested more than " +
                          std::to_string(kMaxDepth) + " levels deep (infinitely recursive generic?)");
            curFile.clear();
            break;
        }
        std::vector<std::pair<std::string, std::string>> params;
        if (!p.selfType.empty()) params.push_back({p.selfType, "self"});
        for (const auto& pr : p.fn->params)
            params.push_back({pr.first == "..." ? pr.first : substType(pr.first, p.subs), pr.second});
        FunctionDecl inst(p.mangled, substType(p.fn->returnType, p.subs), params, p.fn->body);
        inst.line = p.fn->line; inst.col = p.fn->col;
        inst.isAsync = p.fn->isAsync;
        inst.paramEscaping = p.fn->paramEscaping;
        inst.paramPositions = p.fn->paramPositions;
        if (!p.selfType.empty()) {
            if (!inst.paramEscaping.empty()) inst.paramEscaping.insert(inst.paramEscaping.begin(), false);
            if (!inst.paramPositions.empty())
                inst.paramPositions.insert(inst.paramPositions.begin(), {p.fn->line, p.fn->col});
        }

        std::map<Expr*, std::string> instTypes;
        std::swap(expressionTypes, instTypes);
        auto savedNarrowed = narrowedNonNull;
        narrowedNonNull.clear();
        curFile = p.file;
        instSubs = p.subs;
        instContext = p.display;
        instDepth = p.depth;
        instRawReturnType = p.fn->returnType;
        inInstance = true;
        curTemplate = p.selfType.empty() ? p.fn : nullptr;
        inst.accept(this);
        curTemplate = nullptr;
        inInstance = false;
        if (p.fn->isAsync || recordInstanceTypes)
            for (const auto& [e, t] : expressionTypes) instanceExprTypes[e].push_back({p.subs, t});
        instRawReturnType.clear();
        instDepth = 0;
        instContext.clear();
        instSubs.clear();
        curFile.clear();
        narrowedNonNull = savedNarrowed;
        std::swap(expressionTypes, instTypes);
    }
    pendingInstances.clear();
}

// Declaration visitors
void TypeChecker::visit(Program*) {
    // Program node is handled by check() method
    // This is called if someone visits it directly
}
