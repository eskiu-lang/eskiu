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

// ============================================================================

TypeChecker::TypeChecker() {
    pushScope();  // Global scope
}

bool TypeChecker::check(Program* program) {
    hasErrors = false;
    errors.clear();

    // Every `&name` in the program: a global whose address is taken is never narrowed.
    for (const auto& decl : program->declarations) {
        if (auto* fd = dynamic_cast<FunctionDecl*>(decl.get())) astwalk::collectAddressTaken(fd->body.get(), globalAddrTaken);
        else if (auto* vd = dynamic_cast<VarDecl*>(decl.get())) astwalk::collectAddressTaken(vd->initializer.get(), globalAddrTaken);
        else if (auto* sd = dynamic_cast<StructDecl*>(decl.get()))
            for (const auto& m : sd->methods)
                if (auto* mf = dynamic_cast<FunctionDecl*>(m.get())) astwalk::collectAddressTaken(mf->body.get(), globalAddrTaken);
    }

    // First pass: register all struct declarations and function signatures
    std::set<std::string> definedFnBodies;   // names of functions WITH a body, for redefinition
    for (const auto& decl : program->declarations) {
        curFile = decl->sourceFile;
        if (auto enumDecl = dynamic_cast<EnumDecl*>(decl.get())) {
            enumTypes.insert(enumDecl->name);
            if (enumDecl->isADT() && !enumDecl->typeParams.empty()) {
                // Generic algebraic enum (Option<T>): a template; instances are
                // monomorphized on use (normalizeType).
                genericEnumDecls[enumDecl->name] = enumDecl;
                for (size_t i = 0; i < enumDecl->members.size(); ++i)
                    genericVariants[enumDecl->members[i].first] = {enumDecl->name, (int)i};
            } else if (enumDecl->isADT()) {
                // Algebraic enum: a tagged union, not int constants. Register each
                // variant by name -> (enum, tag) for construction + match.
                adtEnums.insert(enumDecl->name);
                enumDecls[enumDecl->name] = enumDecl;
                for (size_t i = 0; i < enumDecl->members.size(); ++i)
                    adtVariants[enumDecl->members[i].first] = {enumDecl->name, (int)i};
            } else {
                foldEnumValues(enumDecl, /*report=*/false);   // provisional: later decls unseen
                for (const auto& m : enumDecl->members) enumConstants[m.first] = m.second;
                plainEnumDecls[enumDecl->name] = enumDecl;   // for exhaustiveness-checked `match`
            }
            continue;
        }
        if (auto aliasDecl = dynamic_cast<TypeAliasDecl*>(decl.get())) {
            typeAliases[aliasDecl->name] = aliasDecl->aliased;
            templateTypeAliases()[aliasDecl->name] = aliasDecl->aliased;
            continue;
        }
        if (auto ifaceDecl = dynamic_cast<InterfaceDecl*>(decl.get())) {
            interfaceDecls[ifaceDecl->name] = ifaceDecl;
            continue;
        }
        if (auto unionDecl = dynamic_cast<UnionDecl*>(decl.get())) {
            // A union is a struct to the type system (all fields at offset 0 in codegen).
            StructInfo info;
            info.name = unionDecl->name;
            info.isUnion = true;
            info.packAlign = unionDecl->packAlign;
            for (const auto& f : unionDecl->fields) {
                info.fields.push_back(f);
                fieldLocations[unionDecl->name + "." + f.name] =
                    {f.line, f.col, unionDecl->sourceFile.empty() ? sourceFile : unionDecl->sourceFile};
            }
            info.pads = unionDecl->pads;
            structs[unionDecl->name] = info;
            continue;
        }
        if (auto funcDecl = dynamic_cast<FunctionDecl*>(decl.get())) {
            fnDeclsAll[funcDecl->name].push_back(funcDecl);
            auto& known = fnDecls[funcDecl->name];
            if (!known.first || funcDecl->body) known = {funcDecl, ""};
            if (!funcDecl->deprecatedMsg.empty()) deprecatedFuncs[funcDecl->name] = funcDecl->deprecatedMsg;
            if (!funcDecl->typeParams.empty()) {
                // A generic prototype (`T f<T>(T x);`) never replaces its definition.
                FunctionDecl*& slot = funcTemplateDecls[funcDecl->name];
                if (!slot || funcDecl->body || !slot->body) slot = funcDecl;
                if (funcDecl->mustUse) mustUseFuncs.insert(funcDecl->name);
                // Don't register yet — template params are not real types,
                // the function is registered on instantiation.
                continue;
            }
        }
        if (auto structDecl = dynamic_cast<StructDecl*>(decl.get())) {
            std::string sfile = structDecl->sourceFile.empty() ? sourceFile : structDecl->sourceFile;
            for (const auto& f : structDecl->fields)
                fieldLocations[structDecl->name + "." + f.name] = {f.line, f.col, sfile};
            for (const auto& method : structDecl->methods)
                if (auto func = dynamic_cast<FunctionDecl*>(method.get())) {
                    fnDecls[structDecl->name + "_" + func->name] = {func, "*" + structDecl->name};
                    fnDeclsAll[structDecl->name + "_" + func->name].push_back(func);
                    methodLocations[structDecl->name + "_" + func->name] = {func->line, func->col, sfile};
                }
            if (!structDecl->typeParams.empty()) {
                templateDecls[structDecl->name] = structDecl;
                continue;
            }

            StructInfo info;
            info.name = structDecl->name;
            info.fields = structDecl->fields;
            info.pads = structDecl->pads;
            info.packAlign = structDecl->isPacked ? 1 : structDecl->packAlign;
            structs[structDecl->name] = info;

            // Register methods as mangled functions: StructName_methodName(self, ...)
            for (const auto& method : structDecl->methods) {
                if (auto func = dynamic_cast<FunctionDecl*>(method.get())) {
                    std::string mangled = structDecl->name + "_" + func->name;
                    std::vector<std::string> paramTypes;
                    paramTypes.push_back("*" + structDecl->name); // implicit self
                    for (const auto& p : func->params) paramTypes.push_back(p.first);
                    defineFunction(mangled, func->returnType, paramTypes);
                }
            }
        } else if (auto funcDecl = dynamic_cast<FunctionDecl*>(decl.get())) {
            std::vector<std::string> paramTypes;
            for (const auto& param : funcDecl->params) {
                paramTypes.push_back(param.first);  // first = type, second = name
            }
            // An async fn's call expression yields *Future<T>; the declared T is
            // the inner type the body returns (the transform wraps it). `async
            // void` uses a 1-byte unit (uint8) as the future's value type.
            std::string sigRet = funcDecl->returnType;
            if (funcDecl->isAsync)
                sigRet = "*Future<" + (funcDecl->returnType == "void" ? std::string("uint8")
                                                                      : funcDecl->returnType) + ">";
            // A second *definition* (body) of the same name is a redefinition; a
            // body-less forward declaration alongside one definition is fine.
            if (!funcDecl->operatorSym.empty()) {
                std::string disp = "operator " + (funcDecl->operatorSym == "u-" ? std::string("-") : funcDecl->operatorSym) + "(";
                for (size_t i = 0; i < funcDecl->params.size(); ++i)
                    disp += (i ? ", " : "") + funcDecl->params[i].first;
                fnDisplayNames[funcDecl->name] = disp + ")";
            }
            if (funcDecl->body) {
                if (definedFnBodies.count(funcDecl->name))
                    errorAt(funcDecl, "redefinition of function '" + fnDisplay(funcDecl->name) + "'");
                definedFnBodies.insert(funcDecl->name);
            }
            defineFunction(funcDecl->name, sigRet, paramTypes);
            functionParamEscaping[funcDecl->name] = funcDecl->paramEscaping;
            if (funcDecl->mustUse) mustUseFuncs.insert(funcDecl->name);
            // Operator overload: index it by op so `a op b` resolves by operand types
            // (with the usual numeric coercions), not by an exact mangled-name match.
            if (!funcDecl->operatorSym.empty())
                operatorOverloads[funcDecl->operatorSym].push_back({paramTypes, funcDecl->returnType, funcDecl->name});
        } else if (auto externDecl = dynamic_cast<ExternDecl*>(decl.get())) {
            std::vector<std::string> paramTypes;
            for (const auto& param : externDecl->params) {
                paramTypes.push_back(param.first);  // first = type, second = name
            }
            defineFunction(externDecl->name, externDecl->returnType, paramTypes);
            functionParamEscaping[externDecl->name] = externDecl->paramEscaping;
            externFnNames.insert(externDecl->name);
        } else if (auto intrinDecl = dynamic_cast<IntrinsicDecl*>(decl.get())) {
            // Intrinsics carry an ordinary signature; only codegen treats them
            // specially (inline lowering instead of a call).
            std::vector<std::string> paramTypes;
            for (const auto& param : intrinDecl->params) {
                paramTypes.push_back(param.first);
            }
            defineFunction(intrinDecl->name, intrinDecl->returnType, paramTypes);
            functionParamEscaping[intrinDecl->name] = intrinDecl->paramEscaping;
        }
    }

    checkTopLevelNames(program);

    // A type alias that resolves back to itself (`type A = B; type B = A;`, `type A = *A;`)
    // names no type. Report it and drop it so type resolution terminates.
    {
        std::map<std::string, TypeAliasDecl*> aliasDecls;
        for (const auto& decl : program->declarations)
            if (auto* ad = dynamic_cast<TypeAliasDecl*>(decl.get())) aliasDecls[ad->name] = ad;
        auto baseName = [](const std::string& t) {
            ty::Type ty = ty::Type::parse(tyq::strip(t));
            while ((ty.kind == ty::Type::Kind::Array || ty.kind == ty::Type::Kind::Slice) && ty.elem) {
                ty::Type e = *ty.elem; ty = e;
            }
            return ty.nominalName();
        };
        // Does identifier `name` occur in type spelling `t` (e.g. as a type argument)?
        auto mentions = [](const std::string& t, const std::string& name) {
            auto idc = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
            for (size_t i = 0; i < t.size();) {
                if (!idc(t[i])) { ++i; continue; }
                size_t j = i;
                while (j < t.size() && idc(t[j])) ++j;
                if (t.compare(i, j - i, name) == 0 && j - i == name.size()) return true;
                i = j;
            }
            return false;
        };
        std::set<std::string> cyclic;
        for (const auto& kv : aliasDecls) {
            std::set<std::string> seen{kv.first};
            std::string cur = kv.first;
            while (true) {
                auto it = typeAliases.find(cur);
                if (it == typeAliases.end()) break;
                std::string next = baseName(it->second);
                // Also through a type argument (`type L = List<L>;`): its layout needs itself.
                if (next == kv.first || mentions(it->second, kv.first)) { cyclic.insert(kv.first); break; }
                if (!seen.insert(next).second) break;   // a cycle not through kv.first
                cur = next;
            }
        }
        for (const auto& n : cyclic) {
            errorAtDecl(aliasDecls[n], "type alias '" + n + "' refers to itself (a cyclic alias names no type)");
        }
        for (const auto& n : cyclic) typeAliases[n] = "unknown";   // resolves to the error sentinel
        for (const auto& n : cyclic) templateTypeAliases()[n] = "unknown";
    }

    // An extern the program also defines is an Eskiu function, not a C one.
    for (const auto& n : definedFnBodies) externFnNames.erase(n);

    // Second pass: type check all declarations
    for (const auto& decl : program->declarations) {
        curFile = decl->sourceFile;
        posCtx = decl.get();
        auto* sm = dynamic_cast<FunctionDecl*>(decl.get());
        bool scriptMain = sm && sm->name == "main" && !sm->params.empty() && sm->params[0].second == "__argc";
        if (scriptMain)
            for (const auto& d : program->declarations)
                if (auto* vd = dynamic_cast<VarDecl*>(d.get());
                    vd && !vd->fromImport && vd->sourceFile == sm->sourceFile && vd->name != "argc" && vd->name != "argv")
                    scriptGlobalPos.emplace(vd->name, std::make_pair(vd->line, vd->col));
        decl->accept(this);
        scriptGlobalPos.clear();
    }
    posCtx = nullptr;
    curFile.clear();
    // Defaults of a function no call reached are checked too.
    for (const auto& kv : fnDeclsAll)
        for (const FunctionDecl* d : kv.second)
            if (!d->paramDefaults.empty()) { paramDefaultsOf(kv.first); break; }

    checkPendingInstances();

    checkValueCycles(program);

    // -Wall: top-level functions defined but never referenced.
    if (warnAll) {
        for (const auto& [name, loc] : definedFns) {
            if (!calledFns.count(name))
                warning(loc.first, loc.second, "unused function '" + fnDisplay(name) + "'");
        }
    }

    // Report errors
    for (const auto& err : errors) {
        std::cerr << "error: " << err << "\n";
    }

    return !hasErrors;
}

// Top-level names share one namespace: a function, global variable, struct/union, enum,
// enum member, interface, or type alias may be declared once. Exceptions: `extern`
// declarations (repeated across modules), a body-less prototype plus its definition (with
// the same signature), a variable's `extern` declaration plus its definition, and a struct
// declared again with identical fields (a multi-file merge of the same declaration).
void TypeChecker::checkTopLevelNames(Program* program) {
    struct Entry { std::string kind; Decl* decl; };
    std::map<std::string, Entry> seen;
    std::map<std::string, std::string> externSigs;   // extern name -> its signature
    auto sameFields = [](const StructDecl* x, const StructDecl* y) {
        auto a = layoutFields(x->fields, x->pads), b = layoutFields(y->fields, y->pads);
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].type != b[i].type || a[i].name != b[i].name || a[i].bitWidth != b[i].bitWidth ||
                a[i].unnamed != b[i].unnamed) return false;
        return true;
    };
    auto sigOf = [&](FunctionDecl* f) {
        std::string s = normalizeType(f->returnType) + "(";
        for (size_t i = 0; i < f->params.size(); ++i) s += (i ? ", " : "") + normalizeType(f->params[i].first);
        return s + ")";
    };
    auto declare = [&](const std::string& name, const std::string& kind, Decl* d) {
        auto it = seen.find(name);
        if (it == seen.end()) { seen[name] = {kind, d}; return; }
        Entry& prev = it->second;
        if (kind == "function" && prev.kind == "function") {
            auto* a = static_cast<FunctionDecl*>(prev.decl);
            auto* b = static_cast<FunctionDecl*>(d);
            if (!a->typeParams.empty() || !b->typeParams.empty()) {
                // A generic function has one definition, and its name is not shared with a
                // non-generic one (a call would silently pick the generic).
                if (a->body && b->body) {
                    errorAtDecl(d, "redefinition of function '" + fnDisplay(name) + "'");
                    return;
                }
                if (a->typeParams.empty() != b->typeParams.empty()) {
                    errorAtDecl(d, "conflicting declaration of function '" + fnDisplay(name) +
                                   "' (a generic and a non-generic function cannot share a name)");
                    return;
                }
            } else if (sigOf(a) != sigOf(b)) {
                errorAtDecl(d, "conflicting declaration of function '" + fnDisplay(name) + "' (" +
                           sigOf(b) + " vs the earlier " + sigOf(a) + ")");
                return;
            }
            if (!prev.decl || (!a->body && b->body)) prev.decl = d;
            return;                                          // two bodies: reported as a redefinition
        }
        if (kind == "variable" && prev.kind == "variable") {
            auto* a = static_cast<VarDecl*>(prev.decl);
            auto* b = static_cast<VarDecl*>(d);
            if ((a->isExtern || b->isExtern) && normalizeType(a->type) == normalizeType(b->type)) return;
            errorAtDecl(d, "redefinition of global variable '" + name + "'");
            return;
        }
        if (kind == "struct" && prev.kind == "struct") {
            auto* a = static_cast<StructDecl*>(prev.decl);
            auto* b = static_cast<StructDecl*>(d);
            if (sameFields(a, b) && a->methods.size() == b->methods.size() &&
                a->typeParams == b->typeParams) return;      // the same declaration, merged twice
            errorAtDecl(d, "redefinition of struct '" + name + "' with different fields");
            return;
        }
        if (kind == prev.kind) {
            errorAtDecl(d, "redefinition of " + kind + " '" + name + "'");
            return;
        }
        // Types and enum members live apart (a struct `G` beside a member `G` is fine);
        // otherwise one name has one meaning.
        auto isType = [](const std::string& k) {
            return k == "struct" || k == "union" || k == "enum" || k == "interface" || k == "type alias";
        };
        if ((isType(kind) && prev.kind == "enum member") || (isType(prev.kind) && kind == "enum member")) return;
        auto article = [](const std::string& k) { return std::string(k[0] == 'e' || k[0] == 'i' || k[0] == 'u' ? "an " : "a ") + k; };
        errorAtDecl(d, "'" + name + "' is declared as both " + article(prev.kind) + " and " + article(kind));
    };
    for (const auto& decl : program->declarations) {
        Decl* d = decl.get();
        if (auto* f = dynamic_cast<FunctionDecl*>(d)) declare(f->name, "function", f);
        else if (auto* v = dynamic_cast<VarDecl*>(d)) declare(v->name, "variable", v);
        else if (auto* s = dynamic_cast<StructDecl*>(d)) {
            declare(s->name, "struct", s);
            // An inline method `S.m` is the function `S_m`; a free `S_m` would be a second one.
            for (const auto& m : s->methods)
                if (auto* mf = dynamic_cast<FunctionDecl*>(m.get())) {
                    std::string mangled = s->name + "_" + mf->name;
                    auto it = seen.find(mangled);
                    if (it != seen.end() && it->second.kind == "function")
                        errorAtDecl(it->second.decl, "function '" + mangled + "' conflicts with method '" +
                                                 mf->name + "' of struct '" + s->name + "'");
                    else seen[mangled] = {"method", mf};
                }
        }
        else if (auto* u = dynamic_cast<UnionDecl*>(d)) declare(u->name, "union", u);
        else if (auto* e = dynamic_cast<EnumDecl*>(d)) {
            declare(e->name, "enum", e);
            // A classic member is an int constant, apart from type names; a sum type's
            // variant is a constructor, so it may not share a name with a type either.
            for (const auto& m : e->members) declare(m.first, e->isADT() ? "variant" : "enum member", e);
        }
        else if (auto* x = dynamic_cast<ExternDecl*>(d)) {
            // An `extern` may be declared again (modules share C functions), but with the
            // same signature: two different ones cannot both describe the C symbol.
            std::string sig = normalizeType(x->returnType) + "(";
            for (size_t i = 0; i < x->params.size(); ++i)
                sig += (i ? ", " : "") + (x->params[i].first == "..." ? std::string("...") : normalizeType(x->params[i].first));
            sig += ")";
            auto [it, fresh] = externSigs.insert({x->name, sig});
            if (!fresh && it->second != sig)
                errorAtDecl(x, "conflicting declaration of extern '" + x->name + "' (" + sig +
                               " vs the earlier " + it->second + ")");
        }
        else if (auto* i = dynamic_cast<InterfaceDecl*>(d)) declare(i->name, "interface", i);
        else if (auto* a = dynamic_cast<TypeAliasDecl*>(d)) {
            auto it = seen.find(a->name);
            if (it != seen.end() && it->second.kind == "type alias" &&
                static_cast<TypeAliasDecl*>(it->second.decl)->aliased == a->aliased) continue;
            declare(a->name, "type alias", a);
        }
    }
    // An extern names a C function, so it cannot share its name with a generic one either
    // (a call would reach the generic, or a mistyped mix of the two).
    for (const auto& decl : program->declarations)
        if (auto* x = dynamic_cast<ExternDecl*>(decl.get())) {
            auto it = seen.find(x->name);
            if (it != seen.end() && it->second.kind == "function" && it->second.decl &&
                !static_cast<FunctionDecl*>(it->second.decl)->typeParams.empty())
                errorAtDecl(x, "conflicting declaration of extern '" + x->name +
                               "' (a generic function has the same name)");
        }
}

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
        if (auto* sd = dynamic_cast<StructDecl*>(decl.get())) {
            VNode n{sd, "struct", sd->typeParams, {}};
            for (const auto& f : sd->fields) n.fields.push_back({f.type, "field '" + f.name + "'"});
            all[sd->name] = n;
        } else if (auto* ud = dynamic_cast<UnionDecl*>(decl.get())) {
            VNode n{ud, "struct", {}, {}};
            for (const auto& f : ud->fields) n.fields.push_back({f.type, "field '" + f.name + "'"});
            all[ud->name] = n;
        } else if (auto* ed = dynamic_cast<EnumDecl*>(decl.get()); ed && ed->isADT()) {
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
        inst.accept(this);
        inInstance = false;
        if (p.fn->isAsync)
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
void TypeChecker::visit(Program* node) {
    // Program node is handled by check() method
    // This is called if someone visits it directly
}

// Approximate the on-screen width of an expression's leading token, so hover can
// require the cursor to actually fall ON the expression rather than merely share
// its line. Identifiers and literals have a clear single-token footprint; for
// composite expressions we use width 1 (match only at their exact start column),
// which lets a more specific child identifier/literal win.
static int hoverSpanWidth(const Expr* e) {
    if (auto* id = dynamic_cast<const IdentExpr*>(e))
        return std::max(1, (int)id->name.size());
    if (auto* lit = dynamic_cast<const LiteralExpr*>(e)) {
        int n = (int)lit->value.size();
        if (lit->kind == LiteralExpr::Kind::STRING ||
            lit->kind == LiteralExpr::Kind::CHAR)
            n += 2;  // surrounding quotes, which the cursor sits within
        return std::max(1, n);
    }
    return 1;
}

static std::string userSpelling(const std::string& msg);   // below, with the diagnostics

std::string TypeChecker::getTypeAtPosition(int line, int col) const {
    // Return a type only when the cursor is within an expression's token span —
    // never for keywords, type annotations, operators, or whitespace. Among the
    // expressions that contain the cursor, prefer the narrowest (most specific).
    int         bestWidth = INT_MAX;
    std::string bestType;
    for (const auto& [node, type] : expressionTypes) {
        if (type == "unknown" || type.empty()) continue;
        if (node->line != line || node->col <= 0) continue;
        int width = hoverSpanWidth(node);
        if (col < node->col || col >= node->col + width) continue;
        if (width < bestWidth) { bestWidth = width; bestType = type; }
    }
    // Declared names (variables, parameters) at their declaration site, and member
    // names (`x.field`, `x.method`).
    // They win over an expression there (a dot-call's receiver argument shares its spot).
    for (const auto& s : hoverSyms) {
        if (s.type.empty() || s.line != line || s.col <= 0) continue;
        int width = std::max(1, s.width);
        if (col < s.col || col >= s.col + width) continue;
        return displayType(s.type);
    }
    // A function's name, where it is used or declared: its signature.
    auto isFn = [&](const std::string& n) { return fnDecls.count(n) || functionSignatures.count(n); };
    for (const auto& [pos, name] : useLocations)
        if (pos.first == line && col >= pos.second && col < pos.second + (int)name.size() && isFn(name))
            return signatureText(name);
    // A function's name at its declaration in this file is no expression of this file, so
    // an expression found at that line and column is one of an imported file (the
    // expression map is not per file): the declaration wins.
    for (const auto& [name, loc] : definitionLocations)
        if (loc.file == sourceFile && loc.line == line && col >= loc.col &&
            col < loc.col + (int)name.size() && isFn(name))
            return signatureText(name);
    return displayType(bestType);
}

std::string TypeChecker::getDefinitionAt(int line, int col) const {
    auto fmtLoc = [](const DefLocation& loc) {
        return loc.file + ":" + std::to_string(loc.line) + ":" + std::to_string(loc.col);
    };
    // 1. A use of a local/parameter whose name spans the cursor: the definition is
    //    the symbol scope resolution picked at that use.
    for (const auto& [pos, use] : useDefs) {
        if (pos.first == line && col >= pos.second && col < pos.second + use.width)
            return fmtLoc(use.def);
    }
    // 2. A member name resolved to a function (`x.method`).
    for (const auto& [pos, use] : memberUses) {
        if (pos.first != line || col < pos.second || col >= pos.second + use.first) continue;
        if (auto ml = methodLocations.find(use.second); ml != methodLocations.end()) return fmtLoc(ml->second);
        auto defit = definitionLocations.find(use.second);
        if (defit != definitionLocations.end()) return fmtLoc(defit->second);
    }
    // 3. A use of a global (function, enum member, global variable) spanning it.
    for (const auto& [pos, name] : useLocations) {
        if (pos.first != line || col < pos.second || col >= pos.second + (int)name.size()) continue;
        auto defit = definitionLocations.find(name);
        if (defit != definitionLocations.end()) return fmtLoc(defit->second);
    }
    // 3. Cursor is directly on a declaration in this file.
    for (const auto& [name, loc] : definitionLocations) {
        if (loc.file == sourceFile && loc.line == line &&
            col >= loc.col && col < loc.col + (int)name.size())
            return fmtLoc(loc);
    }
    for (const auto& s : hoverSyms) {
        if (s.line == line && col >= s.col && col < s.col + std::max(1, s.width))
            return sourceFile + ":" + std::to_string(s.line) + ":" + std::to_string(s.col);
    }
    // 5. An inline method's declaration (`Point_sum`: the name after `Point_`).
    for (const auto& [key, loc] : methodLocations) {
        auto fd = fnDecls.find(key);
        if (fd == fnDecls.end() || fd->second.second.size() < 2) continue;
        int w = (int)(key.size() - fd->second.second.size());
        if (loc.file == sourceFile && loc.line == line && col >= loc.col && col < loc.col + w)
            return fmtLoc(loc);
    }
    // 6. A struct field's declaration ("Struct.field": the name is after the dot).
    for (const auto& [key, loc] : fieldLocations) {
        int w = (int)(key.size() - key.find('.') - 1);
        if (loc.file == sourceFile && loc.line == line && col >= loc.col && col < loc.col + w)
            return fmtLoc(loc);
    }
    return "";
}

std::vector<std::string> TypeChecker::getReferencesAt(int line, int col) const {
    std::string target = getDefinitionAt(line, col);
    if (target.empty()) return {};
    auto fmtLoc = [](const DefLocation& loc) {
        return loc.file + ":" + std::to_string(loc.line) + ":" + std::to_string(loc.col);
    };
    std::map<std::pair<int, int>, int> uses;
    int defWidth = 0;   // the declared name's width: `P_sum` for a dot-called `p.sum()`
    for (const auto& [pos, use] : useDefs)
        if (fmtLoc(use.def) == target) uses[pos] = use.width;
    for (const auto& [pos, use] : memberUses) {
        auto ml = methodLocations.find(use.second);
        auto dl = definitionLocations.find(use.second);
        if (ml != methodLocations.end() && fmtLoc(ml->second) == target) {
            uses[pos] = use.first;
        } else if (ml == methodLocations.end() && dl != definitionLocations.end() && fmtLoc(dl->second) == target) {
            uses[pos] = use.first;
            defWidth = (int)use.second.size();
        }
    }
    for (const auto& [pos, name] : useLocations) {
        auto dl = definitionLocations.find(name);
        if (dl != definitionLocations.end() && fmtLoc(dl->second) == target) {
            uses[pos] = (int)name.size();
            defWidth = (int)name.size();
        }
    }
    int width = 0;
    for (const auto& [pos, w] : uses)
        if (pos.first == line && col >= pos.second && col < pos.second + w) width = w;
    if (!width && !uses.empty()) width = uses.begin()->second;
    if (!defWidth) defWidth = width;
    std::vector<std::string> out;
    // The definition: in this file it sorts with the uses; elsewhere it comes first.
    size_t c2 = target.rfind(':'), c1 = target.rfind(':', c2 - 1);
    std::string dfile = target.substr(0, c1);
    std::pair<int, int> dpos{std::stoi(target.substr(c1 + 1, c2 - c1 - 1)), std::stoi(target.substr(c2 + 1))};
    if (dfile == sourceFile) uses.emplace(dpos, defWidth);
    else out.push_back(target + ":" + std::to_string(defWidth));
    for (const auto& [pos, w] : uses)
        out.push_back(sourceFile + ":" + std::to_string(pos.first) + ":" + std::to_string(pos.second) + ":" + std::to_string(w));
    return out;
}

int TypeChecker::scopeOf(const std::string& name) const {
    auto it = scopeIndex.find(name);
    return it == scopeIndex.end() ? -1 : it->second.back();
}

const TypeChecker::Symbol* TypeChecker::findSymbol(const std::string& name) const {
    if (globalLookupOnly && !scopes.empty()) {
        auto it = scopes[0].find(name);
        return it == scopes[0].end() ? nullptr : &it->second;
    }
    int si = scopeOf(name);
    return si < 0 ? nullptr : &scopes[si].find(name)->second;
}

// Scope management
void TypeChecker::pushScope() {
    scopes.push_back(std::map<std::string, Symbol>());
}

void TypeChecker::popScope() {
    if (scopes.empty()) return;
    // -Wall: warn about names declared in this scope that were never referenced.
    // Skip the global scope (size 1) — unused globals are often intentional.
    if (warnAll && scopes.size() > 1) {
        for (const auto& [name, sym] : scopes.back()) {
            if (sym.used || name == "self" || name == "_") continue;
            warning(sym.line, sym.col,
                    std::string(sym.isParam ? "unused parameter '" : "unused variable '")
                    + name + "'");
        }
    }
    for (const auto& entry : scopes.back()) {
        auto it = scopeIndex.find(entry.first);
        it->second.pop_back();
        if (it->second.empty()) scopeIndex.erase(it);
    }
    scopes.pop_back();
}

bool TypeChecker::isConstSymbol(const std::string& name) const {
    const Symbol* sym = findSymbol(name);
    return sym && sym->isConst;
}

bool TypeChecker::assignsToConst(Expr* lhs, std::string& nameOut) {
    Expr* cur = lhs;
    // Root identifier of an expression, for a readable diagnostic.
    auto rootName = [](Expr* e) -> std::string {
        while (e) {
            if (auto* id = dynamic_cast<IdentExpr*>(e)) return id->name;
            if (auto* u = dynamic_cast<UnaryExpr*>(e))  { e = u->operand.get(); continue; }
            if (auto* m = dynamic_cast<MemberExpr*>(e)) { e = m->base.get();    continue; }
            if (auto* ix = dynamic_cast<IndexExpr*>(e)) { e = ix->base.get();   continue; }
            return "";
        }
        return "";
    };
    while (cur) {
        if (auto* id = dynamic_cast<IdentExpr*>(cur)) {
            if (isConstSymbol(id->name)) { nameOut = id->name; return true; }
            return false;
        }
        // Dereference: `*p = x` writes the pointee — illegal if it is const.
        if (auto* u = dynamic_cast<UnaryExpr*>(cur)) {
            if (u->op == "*" && tyq::baseConst(getPointeeType(getExpressionType(u->operand.get())))) {
                nameOut = rootName(u->operand.get()); return true;
            }
            return false;
        }
        // Member/element of a *value* aggregate keeps the same const root and we
        // keep walking; through a pointer it writes the pointee, which is illegal
        // only if that pointee (the struct/element) is const.
        if (auto* m = dynamic_cast<MemberExpr*>(cur)) {
            std::string bt = getExpressionType(m->base.get());
            if (tyq::isPtr(bt)) {
                if (tyq::baseConst(getPointeeType(bt))) { nameOut = m->member; return true; }
                return false;
            }
            cur = m->base.get(); continue;
        }
        if (auto* ix = dynamic_cast<IndexExpr*>(cur)) {
            std::string bt = getExpressionType(ix->base.get());
            if (tyq::isPtr(bt)) {
                if (tyq::baseConst(getPointeeType(bt))) { nameOut = rootName(ix->base.get()); return true; }
                return false;
            }
            cur = ix->base.get(); continue;
        }
        return false;  // calls, etc. — not an in-place const mutation
    }
    return false;
}

void TypeChecker::defineSymbol(const std::string& name, const std::string& type) {
    defineSymbol(name, type, 0, 0, false);
}

// `type NP = ?*N;`: a use of NP is the nullable pointer itself, so the `?` (which the
// null-safety checks read off the front of a type) must be visible. Other aliases are
// resolved by normalizeType as usual.
std::string TypeChecker::nullableAliasTarget(const std::string& t) {
    std::string cur = t;
    for (int guard = 0; guard < 64; ++guard) {
        auto it = typeAliases.find(cur);
        if (it == typeAliases.end()) return t;
        cur = it->second;
        if (!cur.empty() && cur[0] == '?') return cur;
    }
    return t;
}

void TypeChecker::defineSymbol(const std::string& name, const std::string& type,
                               int line, int col, bool isParam) {
    if (!scopes.empty()) {
        Symbol s;
        s.type = nullableAliasTarget(type); s.isDeclared = true;
        s.used = false; s.line = line; s.col = col; s.isParam = isParam;
        s.file = diagFile();
        auto ins = scopes.back().insert_or_assign(name, s);
        if (ins.second) scopeIndex[name].push_back((int)scopes.size() - 1);
    }
    // Record a hover span for the declared name (col points at the name token).
    // Parameters are excluded: the parser stamps them at the *function's*
    // position, not the parameter's, so a span there would be wrong. Parameter
    // uses inside the body still hover correctly via expression types.
    if (line > 0 && !isParam && inPrimaryFile()) {
        std::string disp = type;
        if (disp.rfind("struct:", 0) == 0) disp = disp.substr(7);  // display "Point", not "struct:Point"
        hoverSyms.push_back({line, col, (int)name.size(), disp});
    }
}

std::string TypeChecker::lookupSymbol(const std::string& name) {
    // The innermost scope that defines it
    int si = scopeOf(name);
    if (si < 0) return "";
    Symbol& sym = scopes[si].find(name)->second;
    sym.used = true;  // -Wall: mark referenced
    return sym.type;
}

void TypeChecker::defineFunction(const std::string& name, const std::string& returnType,
                                  const std::vector<std::string>& paramTypes) {
    functionSignatures[name] = {returnType, paramTypes};
}

// A diagnostic names types as the user writes them: drop the internal `struct:` and
// `interface:` tags the normalized spellings carry.
static std::string userSpelling(const std::string& msg) {
    std::string out;
    out.reserve(msg.size());
    for (size_t i = 0; i < msg.size(); i++) {
        bool boundary = i == 0 || !(std::isalnum((unsigned char)msg[i - 1]) || msg[i - 1] == '_');
        if (boundary && msg.compare(i, 7, "struct:") == 0) { i += 6; continue; }
        if (boundary && msg.compare(i, 10, "interface:") == 0) { i += 9; continue; }
        out += msg[i];
    }
    return out;
}

std::string TypeChecker::displayType(const std::string& t0) const {
    std::string t = userSpelling(t0), out;
    for (size_t i = 0; i < t.size();) {
        if (std::isalpha((unsigned char)t[i]) || t[i] == '_') {
            size_t j = i;
            while (j < t.size() && (std::isalnum((unsigned char)t[j]) || t[j] == '_')) ++j;
            std::string word = t.substr(i, j - i);
            auto ti = templateInstanceArgs.find(word);
            if (ti != templateInstanceArgs.end() && word != ti->second.first) {
                out += ti->second.first + "<";
                for (size_t k = 0; k < ti->second.second.size(); ++k)
                    out += (k ? ", " : "") + displayType(ti->second.second[k]);
                out += ">";
            } else {
                out += word;
            }
            i = j;
        } else {
            out += t[i++];
        }
    }
    return out;
}

std::string TypeChecker::signatureText(const std::string& fn) const {
    std::string s;
    if (auto it = fnDecls.find(fn); it != fnDecls.end()) {
        const FunctionDecl* fd = it->second.first;
        if (fd->isAsync) s += "async ";
        s += fd->returnType + " " + fn;
        if (!fd->typeParams.empty()) {
            s += "<";
            for (size_t i = 0; i < fd->typeParams.size(); ++i) s += (i ? ", " : "") + fd->typeParams[i];
            s += ">";
        }
        s += "(";
        bool first = true;
        if (!it->second.second.empty()) { s += it->second.second + " self"; first = false; }
        for (const auto& [type, name] : fd->params) {
            if (!first) s += ", ";
            first = false;
            s += type == "..." || name.empty() ? type : type + " " + name;
        }
        return userSpelling(s + ")");
    }
    auto sig = functionSignatures.find(fn);
    if (sig == functionSignatures.end()) return "";
    s = sig->second.first + " " + fn + "(";
    for (size_t i = 0; i < sig->second.second.size(); ++i) s += (i ? ", " : "") + sig->second.second[i];
    return userSpelling(s + ")");
}

void TypeChecker::recordMemberTooling(MemberExpr* m, const std::string& hover, const std::string& fnName,
                                      const DefLocation* def) {
    if (!inPrimaryFile() || m->memberLine <= 0) return;
    int width = (int)m->member.size();
    if (!hover.empty()) hoverSyms.push_back({m->memberLine, m->memberCol, width, hover});
    if (def) useDefs[{m->memberLine, m->memberCol}] = {width, *def};
    else if (!fnName.empty()) memberUses[{m->memberLine, m->memberCol}] = {width, fnName};
}

// The members `recv.` can be followed by: the fields of its struct, the functions that
// dot syntax reaches (`T_m` whose first parameter points to T, a generic `G_m` on an
// instance of G) and an interface's methods. `_`-prefixed (internal) names are left out.
void TypeChecker::completeMembers(const std::string& recvType) {
    std::string t = recvType;
    if (t.rfind("const ", 0) == 0) t = t.substr(6);
    if (!t.empty() && t[0] == '?') t = t.substr(1);
    t = dealiasOperand(t);
    if (ty::Type::parse(t).kind == ty::Type::Kind::Slice) {
        completions.push_back({"field", "len", "int64"});
        return;
    }
    std::string nb = ty::Type::parse(t).nominalName();
    if (typeAliases.count(nb)) nb = ty::Type::parse(normalizeType(nb)).nominalName();
    ty::Type rt = ty::Type::parse(tyq::strip(t));
    while (rt.isPointer() && rt.pointee) { ty::Type inner = *rt.pointee; rt = inner; }
    if (rt.isTemplate()) nb = ty::Type::parse(normalizeType(rt.str())).nominalName();
    if (nb.rfind("struct:", 0) == 0) nb = nb.substr(7);
    if (nb.rfind("interface:", 0) == 0) nb = nb.substr(10);   // an interface value's methods
    std::set<std::string> seen;
    auto add = [&](const std::string& kind, const std::string& name, const std::string& detail) {
        if (name.empty() || name[0] == '_' || !seen.insert(name).second) return;
        completions.push_back({kind, name, displayType(detail)});
    };
    if (auto si = structs.find(nb); si != structs.end())
        for (const auto& f : si->second.fields)
            if (!f.unnamed) add("field", f.name, f.type);
    std::string prefix = nb + "_";
    for (auto it = functionSignatures.lower_bound(prefix);
         it != functionSignatures.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
        const auto& pts = it->second.second;
        if (pts.empty() || !tyq::isPtr(pts[0])) continue;
        if (ty::Type::parse(normalizeType(tyq::strip(tyq::pointee(pts[0])))).nominalName() != nb &&
            tyq::strip(tyq::pointee(pts[0])) != nb) continue;
        add("method", it->first.substr(prefix.size()), signatureText(it->first));
    }
    if (auto ti = templateInstanceArgs.find(nb); ti != templateInstanceArgs.end()) {
        std::string gp = ti->second.first + "_";
        for (auto it = funcTemplateDecls.lower_bound(gp);
             it != funcTemplateDecls.end() && it->first.compare(0, gp.size(), gp) == 0; ++it)
            if (!it->second->params.empty())
                add("method", it->first.substr(gp.size()), signatureText(it->first));
        if (auto td = templateDecls.find(ti->second.first); td != templateDecls.end()) {
            for (const auto& f : td->second->fields) add("field", f.name, f.type);
            for (const auto& m : td->second->methods)
                if (auto fd = dynamic_cast<FunctionDecl*>(m.get()))
                    add("method", fd->name, signatureText(ti->second.first + "_" + fd->name));
        }
    }
    if (auto ii = interfaceDecls.find(nb); ii != interfaceDecls.end())
        for (const auto& m : ii->second->methods) {
            std::string d = m.returnType + " " + m.name + "(";
            for (size_t i = 0; i < m.params.size(); ++i)
                d += (i ? ", " : "") + m.params[i].first + (m.params[i].second.empty() ? "" : " " + m.params[i].second);
            add("method", m.name, d + ")");
        }
}

std::string TypeChecker::didYouMean(const std::string& name0, char kind, const std::string& recvType) {
    // A generic type is matched by its name without the arguments (`Opy<int>` -> `Opt`).
    std::string name = kind == 't' ? name0.substr(0, name0.find('<')) : name0;
    if (name.size() < 2 || name == kCompleteSentinel) return "";
    std::vector<std::string> cands;
    if (kind == 't') {
        static const char* prims[] = {"int", "int8", "int16", "int32", "int64", "uint", "uint8", "uint16",
                                      "uint32", "uint64", "float", "double", "bool", "char", "string", "void"};
        for (const char* p : prims) cands.push_back(p);
        for (const auto& [n, s] : structs) if (!templateInstanceArgs.count(n)) cands.push_back(n);
        for (const auto& [n, s] : templateDecls) cands.push_back(n);
        for (const auto& [n, s] : interfaceDecls) cands.push_back(n);
        for (const auto& [n, s] : enumDecls) cands.push_back(n);
        for (const auto& n : enumTypes) cands.push_back(n);          // classic enums
        for (const auto& [n, s] : genericEnumDecls) cands.push_back(n);
        for (const auto& [n, s] : typeAliases) cands.push_back(n);
    } else {
        auto saved = std::move(completions);
        completions.clear();
        if (kind == 'm') completeMembers(recvType); else completeScope();
        for (const auto& c : completions)
            if (kind == 'm' || c.kind != "struct") cands.push_back(c.name);
        completions = std::move(saved);
    }
    auto lower = [](std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; };
    std::string ln = lower(name);
    size_t limit = name.size() <= 3 ? 1 : 2;
    std::string best;
    size_t bestD = limit + 1;
    for (const auto& c : cands) {
        if (c == name || c.empty() || c[0] == '_') continue;
        std::string lc = lower(c);
        if (lc.size() > ln.size() + limit || ln.size() > lc.size() + limit) continue;
        // Optimal string alignment: an adjacent transposition (`cuont`) is one change.
        std::vector<size_t> prev2(lc.size() + 1), prev(lc.size() + 1), cur(lc.size() + 1);
        for (size_t j = 0; j <= lc.size(); ++j) prev[j] = j;
        for (size_t i = 1; i <= ln.size(); ++i) {
            cur[0] = i;
            for (size_t j = 1; j <= lc.size(); ++j) {
                cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ln[i - 1] == lc[j - 1] ? 0 : 1)});
                if (i > 1 && j > 1 && ln[i - 1] == lc[j - 2] && ln[i - 2] == lc[j - 1])
                    cur[j] = std::min(cur[j], prev2[j - 2] + 1);
            }
            std::swap(prev2, prev);
            std::swap(prev, cur);
        }
        size_t d = prev[lc.size()];
        if (lc == ln) d = 0;
        // Two changes need six letters: `List` is not a near miss of `int`.
        if (d >= 2 && d * 3 > std::max(lc.size(), ln.size())) continue;
        if (d < bestD || (d == bestD && c < best)) { bestD = d; best = c; }
    }
    return bestD <= limit ? " (did you mean '" + best + "'?)" : "";
}

// The names visible at an identifier: variables in scope (innermost first), then the
// program's functions, types, enum members and constants.
void TypeChecker::completeScope() {
    std::set<std::string> seen;
    auto add = [&](const std::string& kind, const std::string& name, const std::string& detail) {
        if (name.empty() || name[0] == '_' || name == kCompleteSentinel || !seen.insert(name).second) return;
        completions.push_back({kind, name, displayType(detail)});
    };
    for (size_t i = scopes.size(); i-- > 0;)
        for (const auto& [name, sym] : scopes[i])
            if (i > 0 || (!structs.count(name) && !templateDecls.count(name) && !interfaceDecls.count(name) &&
                          !enumDecls.count(name) && !typeAliases.count(name)))
                add(i == 0 ? "global" : "variable", name, sym.type);
    for (const auto& [name, sig] : functionSignatures) add("function", name, signatureText(name));
    for (const auto& [name, fd] : funcTemplateDecls) add("function", name, signatureText(name));
    for (const auto& [name, si] : structs)
        if (!templateInstanceArgs.count(name)) add("struct", name, si.isUnion ? "union " + name : "struct " + name);
    for (const auto& [name, td] : templateDecls) add("struct", name, "struct " + name);
    for (const auto& [name, id] : interfaceDecls) add("interface", name, "interface " + name);
    for (const auto& [name, ed] : enumDecls) add("enum", name, "enum " + name);
    for (const auto& [name, target] : typeAliases) add("type", name, "type " + name + " = " + target);
    for (const auto& [name, v] : enumConstants) add("enummember", name, std::to_string(v));
    for (const auto& [name, v] : adtVariants) add("enummember", name, v.first);
}

// Error reporting
void TypeChecker::error(int line, int col, const std::string& message) {
    hasErrors = true;
    std::stringstream ss;
    ss << diagFile() << ":" << line << ":" << col << ": " << userSpelling(message);
    if (inInstance) ss << " (in instantiation of " << instContext << ")";
    errors.push_back(ss.str());
}

void TypeChecker::errorAtDecl(Decl* d, const std::string& message) {
    std::string saved = curFile;
    if (!d->sourceFile.empty()) curFile = d->sourceFile;
    errorAt(d, message);
    curFile = saved;
}

void TypeChecker::warning(int line, int col, const std::string& message) {
    if (inInstance || noWarnings) return;   // a generic body is linted once, not once per instance
    // Code from an imported module is a library: not linted, as C compilers treat
    // system headers.
    if (auto* d = dynamic_cast<Decl*>(posCtx); d && d->fromImport) return;
    if (warningsAsErrors) { error(line, col, message); return; }
    std::stringstream ss;
    ss << diagFile() << ":" << line << ":" << col << ": warning: " << userSpelling(message);
    std::cerr << ss.str() << "\n";
}

void TypeChecker::noteDeprecatedUse(const std::string& fn, int line, int col) {
    if (!warnDeprecated || line <= 0) return;
    auto it = deprecatedFuncs.find(fn);
    if (it == deprecatedFuncs.end()) return;
    if (!deprecatedWarned().insert(diagFile() + ":" + std::to_string(line) + ":" + std::to_string(col)).second) return;
    std::string msg = it->second == " " ? "" : ": " + it->second;
    warning(line, col, "'" + fn + "' is deprecated" + msg);
}

void TypeChecker::noteWarning(const std::string& name, int line, int col, const std::string& message) {
    if (!warnDefaults || line <= 0 || disabledWarnings.count(name)) return;
    if (auto* d = dynamic_cast<Decl*>(posCtx); d && d->fromImport) return;
    if (inInstance) return;
    if (!deprecatedWarned().insert(diagFile() + ":" + std::to_string(line) + ":" + std::to_string(col) + ":" + name).second) return;
    warning(line, col, message + " [-W" + name + "]");
}

void TypeChecker::warnAssignInCondition(Expr* cond) {
    if (!warnAll) return;
    if (auto* b = dynamic_cast<BinaryExpr*>(cond); b && b->op == "=")
        warning(b->line, b->col,
                "assignment used as a condition — did you mean '=='?");
}

// A condition may be a bool, a number (`!= 0`) or a pointer, nullable included (`!= null`).
bool TypeChecker::isConditionType(const std::string& type) {
    std::string n = normalizeType(type);   // a classic enum is its int value
    if (type == "unknown" || n == "bool" || isNumericType(type) || isNumericType(n)) return true;
    if (interfaceDecls.count(n)) return true;   // an interface value: true when not null
    return isPointerType((!type.empty() && type[0] == '?') ? type.substr(1) : type);
}

void TypeChecker::checkCondition(ASTNode* node, Expr* cond) {
    cond->accept(this);
    std::string condType = getExpressionType(cond);
    if (!isConditionType(condType))
        errorAt(node, "condition must be boolean, numeric, or a pointer, got " + condType);
}
