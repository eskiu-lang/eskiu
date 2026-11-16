// typecheck_call_args.cpp: generic method calls, format strings, variadic and default arguments (split from typecheck_expr.cpp).
#include "type_checker.h"
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <set>
#include <cstring>
#include <cctype>

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with codegen; see template_utils.h.
#include "../template_utils.h"
#include <climits>
#include <cstdint>
#include "../ast/type_qual.h"
#include "../ast/ast_walk.h"

// `x.m(args)` where x is an instance `S<A..>` of a generic struct and `S_m<T..>` is a
// generic free function taking the receiver first (the stdlib's `Type_method`
// convention, e.g. `List_push<T>(List<T>* self, T item)`): the call is `S_m(&x, args)`
// (or `S_m(x, args)` for a pointer receiver), with the type arguments unified from
// the receiver and then the arguments. Returns false when no such function exists.
bool TypeChecker::checkGenericMethodCall(CallExpr* node, MemberExpr* member, const std::string& baseType) {
    auto ti = templateInstanceArgs.find(baseType);
    if (ti == templateInstanceArgs.end())   // a source-form receiver type (`Chan<int>*`)
        ti = templateInstanceArgs.find(
            ty::Type::parse(normalizeType(getExpressionType(member->base.get()))).nominalName());
    if (ti == templateInstanceArgs.end()) return false;
    std::string fnName = ti->second.first + "_" + member->member;
    auto ft = funcTemplateDecls.find(fnName);
    if (ft == funcTemplateDecls.end() || ft->second->params.empty()) return false;
    FunctionDecl* fd = ft->second;
    appendDefaultArgs(node->args, node->args.size() + 1, fnName, node);
    calledFns.insert(fnName);
    noteDeprecatedUse(fnName, member->memberLine > 0 ? member->memberLine : node->line,
                      member->memberLine > 0 ? member->memberCol : node->col);
    recordMemberTooling(member, signatureText(fnName), fnName);
    for (auto& a : node->args) a->accept(this);

    const std::string& selfT = fd->params[0].first;
    std::string recvT = getExpressionType(member->base.get());
    bool recvPtr = tyq::isPtr(recvT), selfPtr = tyq::isPtr(selfT);
    std::string recvAsSelf = recvT;
    if (selfPtr && !recvPtr) recvAsSelf = "*" + recvT;
    else if (!selfPtr && recvPtr) recvAsSelf = tyq::pointee(recvT);
    if (selfPtr && recvPtr) {                                  // `*Alias` receiver
        std::string pe = tyq::pointee(recvT), de = dealiasOperand(pe);
        if (de != pe) recvAsSelf = "*" + de;
    }

    std::set<std::string> tps(fd->typeParams.begin(), fd->typeParams.end());
    std::map<std::string, std::string> subs;
    unifyTypeParam(selfT, recvAsSelf, tps, subs);
    for (size_t j = 1; j < fd->params.size() && j - 1 < node->args.size(); ++j) {
        std::string at = getExpressionType(node->args[j - 1].get());
        if (at != "unknown" && !at.empty()) unifyTypeParam(fd->params[j].first, at, tps, subs);
    }
    std::string unbound;
    for (const auto& tp : fd->typeParams)
        if (!subs.count(tp)) unbound += (unbound.empty() ? "" : ", ") + tp;
    if (!unbound.empty()) {
        errorAt(node, "cannot infer type argument(s) " + unbound + " of generic function '" +
                      fnName + "' from the method call; call it explicitly, e.g. " + fnName + "<...>(...)");
        expressionTypes[node] = "unknown";
        return true;
    }
    size_t errsBefore = errors.size();
    checkConstraints(node, fd->constraints, subs);
    // A plain (mutable) pointer `self` may write through it, so it cannot take a
    // read-only receiver (a const value, or one reached through a pointer to const).
    if (selfPtr) {
        std::string cname;
        bool roRecv = recvPtr ? tyq::baseConst(tyq::pointee(recvT))
                              : assignsToConst(member->base.get(), cname);
        if (roRecv && !tyq::baseConst(tyq::pointee(selfT)))
            errorAt(node, "cannot call method '" + member->member + "' on a read-only value: '" +
                          fnName + "' takes '" + selfT + "' and may modify it");
        else if (!recvPtr && !tyq::baseConst(tyq::pointee(selfT)))
            checkCapturedMethodCall(node, member);
    }
    std::vector<std::string> pts;
    for (size_t j = 1; j < fd->params.size(); ++j) pts.push_back(substType(fd->params[j].first, subs));
    checkCallArgs(node, "method '" + member->member + "'", pts);
    if (errors.size() == errsBefore) {
        std::string mangled = fnName;
        for (const auto& tpn : fd->typeParams) mangled += "_" + mangleTemplate(subs[tpn]);
        queueInstance(fd, fd->typeParams, subs, fnName, "", mangled, "", fd->sourceFile);
    }
    expressionTypes[node] = genericCallRet(fd, subs);
    return true;
}

// The argument classes a printf conversion can take, after C's default promotions.
enum { FmtI32 = 1, FmtI64, FmtFloat, FmtStr, FmtPtr, FmtOther };

int TypeChecker::formatArgClass(Expr* arg) {
    if (auto* l = nodeCast<LiteralExpr>(arg); l && l->kind == LiteralExpr::Kind::NULL_VAL) return FmtPtr;
    std::string t = normalizeType(tyq::strip(getExpressionType(arg)));
    if (!t.empty() && t[0] == '?') t = t.substr(1);
    static const std::set<std::string> i32 = {"bool", "char", "int8", "uint8", "int16", "uint16",
                                              "int", "int32", "uint", "uint32"};
    if (i32.count(t)) return FmtI32;
    if (t == "int64" || t == "uint64") return FmtI64;
    if (t == "float" || t == "double") return FmtFloat;
    if (t == "string") return FmtStr;
    if (isPointerType(t)) {
        std::string pt = tyq::strip(normalizeType(getPointeeType(t)));
        if (pt == "char" || pt == "uint8" || pt == "int8") return FmtStr;
        return FmtPtr;
    }
    return FmtOther;
}

// printf-style functions: the format argument's index among the call's arguments.
static int formatIndexOf(const std::string& fn) {
    static const std::map<std::string, int> table = {
        {"printf", 0}, {"fprintf", 1}, {"dprintf", 1}, {"sprintf", 1}, {"snprintf", 2},
        {"String_format", 1}, {"String_appendf", 1}, {"log_at", 1}, {"log_trace", 0},
        {"log_debug", 0}, {"log_info", 0}, {"log_warn", 0}, {"log_error", 0}};
    auto it = table.find(fn);
    return it == table.end() ? -1 : it->second;
}

void TypeChecker::checkFormatCall(Expr* call, const std::string& fn, const std::vector<ExprPtr>& args, int shift) {
    int fi = formatIndexOf(fn);
    if (fi < 0) return;
    fi -= shift;
    if (fi < 0 || fi >= (int)args.size()) return;
    auto* lit = nodeCast<LiteralExpr>(args[fi].get());
    if (!lit || lit->kind != LiteralExpr::Kind::STRING) return;
    bool win = targetTriple.empty()
#ifdef _WIN32
        ? true
#else
        ? false
#endif
        : (targetTriple.find("windows") != std::string::npos || targetTriple.find("mingw") != std::string::npos ||
           targetTriple.find("win32") != std::string::npos);
    const std::string& f = lit->value;
    size_t next = fi + 1;
    auto need = [&](int cls, const std::string& conv) {
        if (next >= args.size()) { ++next; return; }
        Expr* a = args[next].get();
        int got = formatArgClass(a);
        ++next;
        if (got == cls || (cls == FmtPtr && got == FmtStr)) return;
        std::string at = normalizeType(getExpressionType(a));
        if (at == "unknown" || at == "void") return;   // already an error
        static const char* what[] = {"", "an int", "a 64-bit integer", "a double", "a string", "a pointer", ""};
        noteWarning("format", call->line, call->col,
                    "format '" + conv + "' expects " + what[cls] + ", but argument " + std::to_string(next + shift) +
                    " has type '" + displayType(getExpressionType(a)) + "'");
    };
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%') continue;
        size_t st = i++;
        if (i < f.size() && f[i] == '%') continue;
        while (i < f.size() && std::strchr("-+ #0'", f[i])) ++i;
        if (i < f.size() && f[i] == '*') { need(FmtI32, "*"); ++i; }
        else while (i < f.size() && std::isdigit((unsigned char)f[i])) ++i;
        if (i < f.size() && f[i] == '.') {
            ++i;
            if (i < f.size() && f[i] == '*') { need(FmtI32, ".*"); ++i; }
            else while (i < f.size() && std::isdigit((unsigned char)f[i])) ++i;
        }
        std::string len;
        while (i < f.size() && std::strchr("hlLqjzt", f[i])) len += f[i++];
        if (i >= f.size()) {
            noteWarning("format", call->line, call->col, "incomplete conversion '" + f.substr(st) + "' at the end of the format string");
            break;
        }
        char c = f[i];
        std::string conv = f.substr(st, i - st + 1);
        bool wide = len == "ll" || len == "j" || len == "z" || len == "t" || len == "q" || (len == "l" && !win);
        if (std::strchr("diouxXc", c)) need(c == 'c' ? FmtI32 : (wide ? FmtI64 : FmtI32), conv);
        else if (std::strchr("fFeEgGaA", c)) need(FmtFloat, conv);
        else if (c == 's') need(FmtStr, conv);
        else if (c == 'p' || c == 'n') need(FmtPtr, conv);
        else {
            noteWarning("format", call->line, call->col, "unknown conversion '" + conv + "' in the format string");
            return;
        }
    }
    if (next > args.size())
        noteWarning("format", call->line, call->col, "the format string needs " + std::to_string(next - fi - 1) +
                    " argument(s), but " + std::to_string(args.size() - fi - 1) + " are given");
    else if (next < args.size())
        noteWarning("format", call->line, call->col, "the format string uses " + std::to_string(next - fi - 1) +
                    " argument(s), but " + std::to_string(args.size() - fi - 1) + " are given");
}

// An argument passed through `...` must have a value: a `void` call has none.
void TypeChecker::checkVariadicArg(Expr* call, Expr* arg, size_t i) {
    if (normalizeType(getExpressionType(arg)) == "void")
        errorAt(call, "argument " + std::to_string(i + 1) + " has type 'void' (a void call has no value)");
}

// A parameter default folded to a constant: a literal, a negative float literal, an
// integer constant expression over literals, enum members and `const` globals (folded in
// the global scope, so a caller's locals never rebind it), or a struct literal whose
// values are all of these (`V3 up = V3{0.0, 1.0, 0.0}`). Null when it is none of them.
namespace {
bool isCallSiteName(const std::string& n) { return n == "src_file" || n == "src_line" || n == "src_col"; }
}  // namespace

bool TypeChecker::callSiteBuiltin(const Expr* e) const {
    auto* c = nodeCast<const CallExpr>(e);
    auto* id = c ? nodeCast<const IdentExpr>(c->callee.get()) : nullptr;
    if (!id || !c->args.empty() || !isCallSiteName(id->name)) return false;
    if (functionSignatures.count(id->name) || funcTemplateDecls.count(id->name)) return false;
    return scopes.empty() || !scopes[0].count(id->name);
}

ExprPtr TypeChecker::foldParamDefault(Expr* def) {
    if (auto* l = nodeCast<LiteralExpr>(def)) return std::make_shared<LiteralExpr>(l->kind, l->value);
    // `int line = src_line()`: kept as is, it becomes the call's line at each call.
    if (callSiteBuiltin(def))
        return std::make_shared<CallExpr>(std::make_shared<IdentExpr>(static_cast<IdentExpr*>(static_cast<CallExpr*>(def)->callee.get())->name));
    auto* u = nodeCast<UnaryExpr>(def);
    auto* ul = u && u->op == "-" ? nodeCast<LiteralExpr>(u->operand.get()) : nullptr;
    if (ul && ul->kind == LiteralExpr::Kind::FLOAT) return std::make_shared<LiteralExpr>(ul->kind, "-" + ul->value);
    if (auto* si = nodeCast<StructInitExpr>(def)) {
        std::vector<std::pair<std::string, ExprPtr>> fields;
        for (auto& f : si->fieldInits) {
            ExprPtr v = f.second ? foldParamDefault(f.second.get()) : nullptr;
            if (!v) return nullptr;
            v->line = f.second->line;
            v->col = f.second->col;
            fields.emplace_back(f.first, v);
        }
        auto out = std::make_shared<StructInitExpr>(si->structName, std::move(fields));
        out->line = si->line;
        out->col = si->col;
        return out;
    }
    long long v = 0;
    globalLookupOnly = true;
    bool ok = foldConstInt(def, v);
    globalLookupOnly = false;
    if (ok) return std::make_shared<LiteralExpr>(LiteralExpr::Kind::INT, std::to_string(v));
    return nullptr;
}

namespace {
// The position a call reports to `src_line()` / `src_col()`: the called name (`f` in
// `f(x)` and in `x.f()`), or the call's own position for `f<T>(x)`.
struct CallSite {
    std::string file;
    int line = 0, col = 0;
};

// A fresh copy of a folded default (a literal, or a struct literal of them) at a call;
// a `src_file()` / `src_line()` / `src_col()` in it becomes `site`.
ExprPtr cloneParamDefault(const Expr* d, int line, int col, const CallSite& site) {
    ExprPtr out;
    if (auto* l = nodeCast<const LiteralExpr>(d)) {
        out = std::make_shared<LiteralExpr>(l->kind, l->value);
    } else if (auto* c = nodeCast<const CallExpr>(d)) {
        const std::string& n = static_cast<const IdentExpr*>(c->callee.get())->name;
        if (n == "src_file") out = std::make_shared<LiteralExpr>(LiteralExpr::Kind::STRING, site.file);
        else out = std::make_shared<LiteralExpr>(LiteralExpr::Kind::INT, std::to_string(n == "src_line" ? site.line : site.col));
    } else if (auto* si = nodeCast<const StructInitExpr>(d)) {
        std::vector<std::pair<std::string, ExprPtr>> fields;
        for (const auto& f : si->fieldInits) fields.emplace_back(f.first, cloneParamDefault(f.second.get(), line, col, site));
        out = std::make_shared<StructInitExpr>(si->structName, std::move(fields));
    }
    if (out) {                         // line 0: each node keeps its own position
        out->line = line > 0 ? line : d->line;
        out->col = line > 0 ? col : d->col;
    }
    return out;
}
}  // namespace

// The folded default values of function `fn`'s parameters (see paramDefaultCache and
// foldParamDefault); each call site gets a fresh copy. Errors are reported once, at the
// default.
const std::vector<ExprPtr>& TypeChecker::paramDefaultsOf(const std::string& fn) {
    auto cached = paramDefaultCache.find(fn);
    if (cached != paramDefaultCache.end()) return cached->second;
    auto& out = paramDefaultCache[fn];
    auto all = fnDeclsAll.find(fn);
    if (all == fnDeclsAll.end()) return out;
    std::string saved = curFile;
    const FunctionDecl* fd = nullptr;
    for (const FunctionDecl* d : all->second) {
        if (d->paramDefaults.empty()) continue;
        if (fd) {
            if (!d->sourceFile.empty()) curFile = d->sourceFile;
            errorAt(const_cast<FunctionDecl*>(d), "default values of '" + fn +
                    "' are given twice (give them on one declaration)");
            curFile = saved;
            continue;
        }
        fd = d;
    }
    if (!fd) return out;
    if (!fd->sourceFile.empty()) curFile = fd->sourceFile;
    const std::string self = fnDecls[fn].second;
    bool genericOwner = !self.empty() && templateDecls.count(self.substr(1));
    if (!self.empty()) out.push_back(nullptr);
    bool seen = false;
    for (size_t i = 0; i < fd->params.size(); ++i) {
        Expr* def = i < fd->paramDefaults.size() ? fd->paramDefaults[i].get() : nullptr;
        const std::string& pname = fd->params[i].second;
        if (fd->params[i].first == "...") {
            if (seen) errorAt(const_cast<FunctionDecl*>(fd), "'" + fn +
                              "' is variadic: its parameters cannot have default values");
            out.push_back(nullptr);
            continue;
        }
        if (!def) {
            if (seen) errorAt(const_cast<FunctionDecl*>(fd), "parameter '" + pname +
                              "' needs a default value: it follows a parameter that has one");
            out.push_back(nullptr);
            continue;
        }
        seen = true;
        // The names a default reads are uses (definition, references, rename).
        if (fd->sourceFile.empty() || fd->sourceFile == sourceFile) {
            std::function<void(Expr*)> noteUses = [&](Expr* e) {
                if (auto* id = nodeCast<IdentExpr>(e)) {
                    if (id->line > 0) useLocations[{id->line, id->col}] = id->name;
                    return;
                }
                astwalk::forEachChildExpr(e, [&](ExprPtr& c) { if (c) noteUses(c.get()); });
            };
            noteUses(def);
        }
        if (genericOwner) {
            errorAt(def, "a method of generic struct '" + self.substr(1) + "' cannot have default values");
            out.push_back(nullptr);
            continue;
        }
        ExprPtr lit = foldParamDefault(def);
        if (!lit) {
            errorAt(def, "default value of parameter '" + pname +
                         "' is not a constant (a literal, an integer constant expression or a struct literal of those)");
            out.push_back(nullptr);
            continue;
        }
        if (fd->typeParams.empty()) {   // its type is checked where it is declared
            ExprPtr probe = cloneParamDefault(lit.get(), 0, 0, CallSite{diagFile(), def->line, def->col});
            probe->accept(this);
            std::string e = assignabilityError(fd->params[i].first, getExpressionType(probe.get()), probe.get());
            if (!e.empty()) errorAt(def, "default value of parameter '" + pname + "': " + e);
        }
        out.push_back(lit);
    }
    curFile = saved;
    return out;
}

// A call that omits trailing arguments with default values gets a copy of each (at the
// call's position): `connect("h")` is `connect("h", 443)`. Direct calls of a function
// (not hidden by a variable) here; a dot-call once its method is resolved (`given`
// then counts the receiver).
void TypeChecker::fillDefaultArgs(CallExpr* node, const std::string& method) {
    std::string fn = method;
    size_t given = node->args.size() + (method.empty() ? 0 : 1);
    if (method.empty()) {
        auto* id = nodeCast<IdentExpr>(node->callee.get());
        if (!id || !lookupSymbol(id->name).empty()) return;
        fn = id->name;
    }
    appendDefaultArgs(node->args, given, fn, node);
}

// The defaults of `fn`'s parameters from `given` on, appended to `args` at `at`'s
// position (a call, or an explicit generic call `f<int>(x)`).
void TypeChecker::appendDefaultArgs(std::vector<ExprPtr>& args, size_t given, const std::string& fn, const Expr* at) {
    if (!fnDeclsAll.count(fn)) return;
    const auto& defs = paramDefaultsOf(fn);
    if (given >= defs.size()) return;
    for (size_t i = given; i < defs.size(); ++i)
        if (!defs[i]) return;
    CallSite site{diagFile(), at->line, at->col};
    if (auto* c = nodeCast<const CallExpr>(at); c && c->callee && c->callee->line > 0) {
        auto* m = nodeCast<const MemberExpr>(c->callee.get());
        site.line = m && m->memberLine > 0 ? m->memberLine : c->callee->line;
        site.col = m && m->memberLine > 0 ? m->memberCol : c->callee->col;
    }
    for (size_t i = given; i < defs.size(); ++i) args.push_back(cloneParamDefault(defs[i].get(), at->line, at->col, site));
}

void TypeChecker::checkCallArgs(CallExpr* node, const std::string& what,
                                const std::vector<std::string>& paramTypes) {
    bool variadic = !paramTypes.empty() && paramTypes.back() == "...";
    size_t fixed = variadic ? paramTypes.size() - 1 : paramTypes.size();
    if (variadic ? node->args.size() < fixed : node->args.size() != fixed) {
        errorAt(node, what + " expects " + (variadic ? "at least " : "") + std::to_string(fixed) +
                      " argument(s), got " + std::to_string(node->args.size()));
        return;
    }
    for (size_t i = 0; i < fixed; ++i) {
        std::string at = getExpressionType(node->args[i].get());
        std::string e = assignabilityError(paramTypes[i], at, node->args[i].get());
        if (!e.empty())
            errorAt(node, "argument " + std::to_string(i + 1) + " type mismatch: expected " +
                          paramTypes[i] + ", got " + at + " (" + e + ")");
    }
    if (variadic)
        for (size_t i = fixed; i < node->args.size(); ++i) checkVariadicArg(node, node->args[i].get(), i);
}

std::string TypeChecker::checkFnValueCall(CallExpr* node, const std::string& what, const std::string& fnType) {
    ty::Type ft = ty::Type::parse(normalizeType(fnType));
    if (!ft.isFn()) return "unknown";
    std::vector<std::string> pts;
    for (const auto& p : ft.params) pts.push_back(p.str());
    checkCallArgs(node, what, pts);
    return ft.ret ? normalizeType(ft.ret->str()) : "unknown";
}

// An integer literal argument of a variant constructor must fit its payload type, as for
// a function argument (`A(300)` for `A(int8)`).
void TypeChecker::checkVariantLiteral(ASTNode* node, const std::string& variant, size_t i,
                                      const std::string& payloadType) {
    Expr* arg = nullptr;
    if (auto* c = nodeCast<CallExpr>(node)) arg = c->args[i].get();
    else if (auto* t = nodeCast<TemplateCallExpr>(node)) arg = t->args[i].get();
    auto* lit = nodeCast<LiteralExpr>(arg);
    std::string pt = normalizeType(payloadType);
    if (lit && lit->kind == LiteralExpr::Kind::INT && isIntType(pt) && !intLiteralFits(pt, lit))
        errorAt(node, "variant '" + variant + "' argument " + std::to_string(i + 1) + ": integer literal " +
                      lit->value + " is out of range for '" + payloadType + "'");
}
