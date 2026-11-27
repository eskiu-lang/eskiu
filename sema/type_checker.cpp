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
    noteTypeDecls(program);

    // Every `&name` in the program: a global whose address is taken is never narrowed.
    for (const auto& decl : program->declarations) {
        if (auto* fd = nodeCast<FunctionDecl>(decl.get())) astwalk::collectAddressTaken(fd->body.get(), globalAddrTaken);
        else if (auto* vd = nodeCast<VarDecl>(decl.get())) astwalk::collectAddressTaken(vd->initializer.get(), globalAddrTaken);
        else if (auto* sd = nodeCast<StructDecl>(decl.get()))
            for (const auto& m : sd->methods)
                if (auto* mf = nodeCast<FunctionDecl>(m.get())) astwalk::collectAddressTaken(mf->body.get(), globalAddrTaken);
    }

    // First pass: register all struct declarations and function signatures
    std::set<std::string> definedFnBodies;   // names of functions WITH a body, for redefinition
    for (const auto& decl : program->declarations) {
        curFile = decl->sourceFile;
        if (auto enumDecl = nodeCast<EnumDecl>(decl.get())) {
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
        if (auto aliasDecl = nodeCast<TypeAliasDecl>(decl.get())) {
            typeAliases[aliasDecl->name] = aliasDecl->aliased;
            templateTypeAliases()[aliasDecl->name] = aliasDecl->aliased;
            continue;
        }
        if (auto ifaceDecl = nodeCast<InterfaceDecl>(decl.get())) {
            interfaceDecls[ifaceDecl->name] = ifaceDecl;
            continue;
        }
        if (auto unionDecl = nodeCast<UnionDecl>(decl.get())) {
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
        if (auto funcDecl = nodeCast<FunctionDecl>(decl.get())) {
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
        if (auto structDecl = nodeCast<StructDecl>(decl.get())) {
            std::string sfile = structDecl->sourceFile.empty() ? sourceFile : structDecl->sourceFile;
            for (const auto& f : structDecl->fields)
                fieldLocations[structDecl->name + "." + f.name] = {f.line, f.col, sfile};
            for (const auto& method : structDecl->methods)
                if (auto func = nodeCast<FunctionDecl>(method.get())) {
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
                if (auto func = nodeCast<FunctionDecl>(method.get())) {
                    std::string mangled = structDecl->name + "_" + func->name;
                    std::vector<std::string> paramTypes;
                    paramTypes.push_back("*" + structDecl->name); // implicit self
                    for (const auto& p : func->params) paramTypes.push_back(p.first);
                    defineFunction(mangled, func->returnType, paramTypes);
                }
            }
        } else if (auto funcDecl = nodeCast<FunctionDecl>(decl.get())) {
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
            if (!funcDecl->operatorSym.empty()) {
                std::string disp = "operator " + (funcDecl->operatorSym == "u-" ? std::string("-") : funcDecl->operatorSym) + "(";
                for (size_t i = 0; i < funcDecl->params.size(); ++i)
                    disp += (i ? ", " : "") + funcDecl->params[i].first;
                fnDisplayNames[funcDecl->name] = disp + ")";
            }
            if (funcDecl->body) definedFnBodies.insert(funcDecl->name);   // two: checkTopLevelNames
            defineFunction(funcDecl->name, sigRet, paramTypes);
            functionParamEscaping[funcDecl->name] = funcDecl->paramEscaping;
            if (funcDecl->mustUse) mustUseFuncs.insert(funcDecl->name);
            noteFormatFunction(funcDecl->name, funcDecl->params);
            // Operator overload: index it by op so `a op b` resolves by operand types
            // (with the usual numeric coercions), not by an exact mangled-name match.
            if (!funcDecl->operatorSym.empty())
                operatorOverloads[funcDecl->operatorSym].push_back({paramTypes, funcDecl->returnType, funcDecl->name});
        } else if (auto externDecl = nodeCast<ExternDecl>(decl.get())) {
            std::vector<std::string> paramTypes;
            for (const auto& param : externDecl->params) {
                paramTypes.push_back(param.first);  // first = type, second = name
            }
            defineFunction(externDecl->name, externDecl->returnType, paramTypes);
            functionParamEscaping[externDecl->name] = externDecl->paramEscaping;
            externFnNames.insert(externDecl->name);
            noteFormatFunction(externDecl->name, externDecl->params);
        } else if (auto intrinDecl = nodeCast<IntrinsicDecl>(decl.get())) {
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
            if (auto* ad = nodeCast<TypeAliasDecl>(decl.get())) aliasDecls[ad->name] = ad;
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
        auto* sm = nodeCast<FunctionDecl>(decl.get());
        bool scriptMain = sm && sm->name == "main" && !sm->params.empty() && sm->params[0].second == "__argc";
        if (scriptMain)
            for (const auto& d : program->declarations)
                if (auto* vd = nodeCast<VarDecl>(d.get());
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
    finalizeGenericEscapes();

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
    std::map<std::string, std::pair<std::string, Decl*>> externSigs;   // extern name -> its signature, first decl
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
        // Every clash names the earlier declaration (it may be in another module).
        auto clash = [&](const std::string& message, const std::string& shown) {
            errorAtDecl(d, message);
            noteFirstDefined(prev.decl, name, shown);
        };
        if (kind == "function" && prev.kind == "function") {
            auto* a = static_cast<FunctionDecl*>(prev.decl);
            auto* b = static_cast<FunctionDecl*>(d);
            std::string shown = fnDisplay(name);
            if (!a->typeParams.empty() || !b->typeParams.empty()) {
                // A generic function has one definition, and its name is not shared with a
                // non-generic one (a call would silently pick the generic).
                if (a->body && b->body) {
                    clash("redefinition of function '" + shown + "'", shown);
                    return;
                }
                if (a->typeParams.empty() != b->typeParams.empty()) {
                    clash("conflicting declaration of function '" + shown +
                          "' (a generic and a non-generic function cannot share a name)", shown);
                    return;
                }
            } else if (sigOf(a) != sigOf(b)) {
                clash("conflicting declaration of function '" + shown + "' (" +
                      sigOf(b) + " vs the earlier " + sigOf(a) + ")", shown);
                return;
            } else if (a->body && b->body) {
                clash("redefinition of function '" + shown + "'", shown);
                return;
            }
            if (!prev.decl || (!a->body && b->body)) prev.decl = d;
            return;
        }
        if (kind == "variable" && prev.kind == "variable") {
            auto* a = static_cast<VarDecl*>(prev.decl);
            auto* b = static_cast<VarDecl*>(d);
            if ((a->isExtern || b->isExtern) && normalizeType(a->type) == normalizeType(b->type)) return;
            clash("redefinition of global variable '" + name + "'", "");
            return;
        }
        if (kind == "struct" && prev.kind == "struct") {
            auto* a = static_cast<StructDecl*>(prev.decl);
            auto* b = static_cast<StructDecl*>(d);
            if (sameFields(a, b) && a->methods.size() == b->methods.size() &&
                a->typeParams == b->typeParams) return;      // the same declaration, merged twice
            clash("redefinition of struct '" + name + "' with different fields", "");
            return;
        }
        if (kind == prev.kind) {
            clash("redefinition of " + kind + " '" + name + "'", "");
            return;
        }
        // Types and enum members live apart (a struct `G` beside a member `G` is fine);
        // otherwise one name has one meaning.
        auto isType = [](const std::string& k) {
            return k == "struct" || k == "union" || k == "enum" || k == "interface" || k == "type alias";
        };
        if ((isType(kind) && prev.kind == "enum member") || (isType(prev.kind) && kind == "enum member")) return;
        auto article = [](const std::string& k) { return std::string(k[0] == 'e' || k[0] == 'i' || k[0] == 'u' ? "an " : "a ") + k; };
        clash("'" + name + "' is declared as both " + article(prev.kind) + " and " + article(kind), "");
    };
    for (const auto& decl : program->declarations) {
        Decl* d = decl.get();
        if (auto* f = nodeCast<FunctionDecl>(d)) declare(f->name, "function", f);
        else if (auto* v = nodeCast<VarDecl>(d)) declare(v->name, "variable", v);
        else if (auto* s = nodeCast<StructDecl>(d)) {
            declare(s->name, "struct", s);
            // An inline method `S.m` is the function `S_m`; a free `S_m` would be a second one.
            for (const auto& m : s->methods)
                if (auto* mf = nodeCast<FunctionDecl>(m.get())) {
                    std::string mangled = s->name + "_" + mf->name;
                    auto it = seen.find(mangled);
                    if (it != seen.end() && it->second.kind == "function")
                        errorAtDecl(it->second.decl, "function '" + mangled + "' conflicts with method '" +
                                                 mf->name + "' of struct '" + s->name + "'");
                    else seen[mangled] = {"method", mf};
                }
        }
        else if (auto* u = nodeCast<UnionDecl>(d)) declare(u->name, "union", u);
        else if (auto* e = nodeCast<EnumDecl>(d)) {
            declare(e->name, "enum", e);
            // A classic member is an int constant, apart from type names; a sum type's
            // variant is a constructor, so it may not share a name with a type either.
            for (const auto& m : e->members) declare(m.first, e->isADT() ? "variant" : "enum member", e);
        }
        else if (auto* x = nodeCast<ExternDecl>(d)) {
            // An `extern` may be declared again (modules share C functions), but with the
            // same signature: two different ones cannot both describe the C symbol.
            std::string sig = normalizeType(x->returnType) + "(";
            for (size_t i = 0; i < x->params.size(); ++i)
                sig += (i ? ", " : "") + (x->params[i].first == "..." ? std::string("...") : normalizeType(x->params[i].first));
            sig += ")";
            auto [it, fresh] = externSigs.insert({x->name, {sig, x}});
            if (!fresh && it->second.first != sig) {
                errorAtDecl(x, "conflicting declaration of extern '" + x->name + "' (" + sig +
                               " vs the earlier " + it->second.first + ")");
                noteFirstDefined(it->second.second, x->name);
            }
        }
        else if (auto* i = nodeCast<InterfaceDecl>(d)) declare(i->name, "interface", i);
        else if (auto* a = nodeCast<TypeAliasDecl>(d)) {
            auto it = seen.find(a->name);
            if (it != seen.end() && it->second.kind == "type alias" &&
                static_cast<TypeAliasDecl*>(it->second.decl)->aliased == a->aliased) continue;
            declare(a->name, "type alias", a);
        }
    }
    // An extern names a C function, so it cannot share its name with a generic one either
    // (a call would reach the generic, or a mistyped mix of the two).
    for (const auto& decl : program->declarations)
        if (auto* x = nodeCast<ExternDecl>(decl.get())) {
            auto it = seen.find(x->name);
            if (it != seen.end() && it->second.kind == "function" && it->second.decl &&
                !static_cast<FunctionDecl*>(it->second.decl)->typeParams.empty()) {
                errorAtDecl(x, "conflicting declaration of extern '" + x->name +
                               "' (a generic function has the same name)");
                noteFirstDefined(it->second.decl, x->name);
            }
        }
}
