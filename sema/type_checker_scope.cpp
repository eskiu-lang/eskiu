// type_checker_scope.cpp: editor queries, scopes, symbols, signatures and diagnostics (split from type_checker.cpp).
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

// Approximate the on-screen width of an expression's leading token, so hover can
// require the cursor to actually fall ON the expression rather than merely share
// its line. Identifiers and literals have a clear single-token footprint; for
// composite expressions we use width 1 (match only at their exact start column),
// which lets a more specific child identifier/literal win.
static int hoverSpanWidth(const Expr* e) {
    if (auto* id = nodeCast<const IdentExpr>(e))
        return std::max(1, (int)id->name.size());
    if (auto* lit = nodeCast<const LiteralExpr>(e)) {
        int n = (int)lit->value.size();
        if (lit->kind == LiteralExpr::Kind::STRING ||
            lit->kind == LiteralExpr::Kind::CHAR)
            n += 2;  // surrounding quotes, which the cursor sits within
        return std::max(1, n);
    }
    return 1;
}

static std::string userSpelling(const std::string& msg);   // below, with the diagnostics

void TypeChecker::noteTypeDecls(const Program* program) {
    typeRefs = program->typeRefs;
    for (const auto& d : program->declarations) {
        std::string detail;
        if (nodeCast<StructDecl>(d.get())) detail = "struct " + d->name;
        else if (nodeCast<UnionDecl>(d.get())) detail = "union " + d->name;
        else if (nodeCast<EnumDecl>(d.get())) detail = "enum " + d->name;
        else if (nodeCast<InterfaceDecl>(d.get())) detail = "interface " + d->name;
        else if (auto* a = nodeCast<TypeAliasDecl>(d.get())) detail = "type " + a->name + " = " + a->aliased;
        else continue;
        if (d->line <= 0) continue;
        typeDefs.emplace(d->name, TypeDef{{d->line, d->col, d->sourceFile.empty() ? sourceFile : d->sourceFile}, detail});
    }
}

const TypeChecker::TypeDef* TypeChecker::typeDefAt(int line, int col) const {
    for (const auto& r : typeRefs) {
        if (r.line != line || col < r.col || col >= r.col + (int)r.name.size()) continue;
        auto it = typeDefs.find(r.name);
        if (it != typeDefs.end()) return &it->second;
    }
    for (const auto& [name, t] : typeDefs)
        if (t.loc.file == sourceFile && t.loc.line == line && col >= t.loc.col && col < t.loc.col + (int)name.size())
            return &t;
    return nullptr;
}

std::string TypeChecker::getTypeAtPosition(int line, int col) const {
    if (const TypeDef* t = typeDefAt(line, col)) return t->detail;
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
    // 0. A type name, where it is written or declared.
    if (const TypeDef* t = typeDefAt(line, col)) return fmtLoc(t->loc);
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
    for (const auto& r : typeRefs) {
        auto td = typeDefs.find(r.name);
        if (td != typeDefs.end() && fmtLoc(td->second.loc) == target) {
            uses[{r.line, r.col}] = (int)r.name.size();
            defWidth = (int)r.name.size();
        }
    }
    if (!defWidth)
        for (const auto& [name, t] : typeDefs)
            if (fmtLoc(t.loc) == target) defWidth = (int)name.size();
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
            // `...` (a variadic function's rest) is read through va_list, never by name.
            if (sym.used || name == "self" || name == "_" || name == "...") continue;
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
            if (auto* id = nodeCast<IdentExpr>(e)) return id->name;
            if (auto* u = nodeCast<UnaryExpr>(e))  { e = u->operand.get(); continue; }
            if (auto* m = nodeCast<MemberExpr>(e)) { e = m->base.get();    continue; }
            if (auto* ix = nodeCast<IndexExpr>(e)) { e = ix->base.get();   continue; }
            return "";
        }
        return "";
    };
    while (cur) {
        if (auto* id = nodeCast<IdentExpr>(cur)) {
            if (isConstSymbol(id->name)) { nameOut = id->name; return true; }
            return false;
        }
        // Dereference: `*p = x` writes the pointee — illegal if it is const.
        if (auto* u = nodeCast<UnaryExpr>(cur)) {
            if (u->op == "*" && tyq::baseConst(getPointeeType(getExpressionType(u->operand.get())))) {
                nameOut = rootName(u->operand.get()); return true;
            }
            return false;
        }
        // Member/element of a *value* aggregate keeps the same const root and we
        // keep walking; through a pointer it writes the pointee, which is illegal
        // only if that pointee (the struct/element) is const.
        if (auto* m = nodeCast<MemberExpr>(cur)) {
            std::string bt = getExpressionType(m->base.get());
            if (tyq::isPtr(bt)) {
                if (tyq::baseConst(getPointeeType(bt))) { nameOut = m->member; return true; }
                return false;
            }
            cur = m->base.get(); continue;
        }
        if (auto* ix = nodeCast<IndexExpr>(cur)) {
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
                if (auto fd = nodeCast<FunctionDecl>(m.get()))
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
    // Code from an imported module is a library's and is not linted, as C compilers treat
    // system headers; a file of the program's own (under lintRoot) is.
    if (auto* d = dynamic_cast<Decl*>(posCtx); d && !lintsDecl(d)) return;
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
    if (auto* d = dynamic_cast<Decl*>(posCtx); d && !lintsDecl(d)) return;
    if (inInstance) return;
    if (!deprecatedWarned().insert(diagFile() + ":" + std::to_string(line) + ":" + std::to_string(col) + ":" + name).second) return;
    warning(line, col, message + " [-W" + name + "]");
}

void TypeChecker::warnAssignInCondition(Expr* cond) {
    if (!warnAll) return;
    if (auto* b = nodeCast<BinaryExpr>(cond); b && b->op == "=")
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
