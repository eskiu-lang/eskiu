// typecheck_builtin.cpp: sizeof, await, closures, threads, template calls, alloc_with and literals (split from typecheck_expr.cpp).
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

void TypeChecker::visit(SizeofExpr* node) {
    if (node->operand) {
        // `sizeof(expr)`: the size of the operand's type; the operand is checked, never
        // evaluated. Outside a generic body the node becomes `sizeof(<that type>)`, so
        // the later phases (and the async lowering) see a plain type operand. Then
        // the names the operand read are not captured by an enclosing lambda: the
        // rewritten node reads none (as a second check of the program would find).
        auto capturesBefore = captureStack;
        node->operand->accept(this);
        std::string t = getExpressionType(node->operand.get());
        if (t == "void" || isVoidValueType(t))
            errorAt(node, "sizeof of 'void': a void value has no size");
        if (!inInstance && t != "unknown" && !t.empty()) {
            node->typeName = tyq::strip(t);
            node->operand = nullptr;
            captureStack = std::move(capturesBefore);
        }
        expressionTypes[node] = "int64";
        return;
    }
    // `sizeof(x)` of a variable measures the variable's type (C semantics). The parser
    // reads the operand as a type spelling, so resolve a bare name that is a variable
    // (not a type) here and rewrite the operand to that variable's type for codegen.
    const std::string tn = node->typeName;
    std::string resolved = tn;
    bool isTypeName = isPrimitiveType(tn) || structs.count(tn) || typeAliases.count(tn) ||
                      enumTypes.count(tn) || interfaceDecls.count(tn) || tn == "va_list";
    if (!isTypeName) {
        std::string vt = lookupSymbol(tn);
        if (!vt.empty() && vt != "unknown" && vt != "struct:" + tn) resolved = tyq::strip(vt);
    }
    if (!inInstance) node->typeName = resolved;   // a generic body's nodes are shared by its instances
    validateStructType(normalizeType(resolved), node);
    if (isVoidValueType(resolved))
        errorAt(node, "sizeof of 'void': a void value has no size");
    expressionTypes[node] = "int64";
}

void TypeChecker::visit(AwaitExpr* node) {
    // The callee may assign any global: a global's narrowing ends after the call.
    struct GlobalNarrowDrop { TypeChecker* t; ~GlobalNarrowDrop() { t->dropGlobalNarrowings(); } } dropAfter{this};
    if (!inAsyncFn)
        errorAt(node, inTrailingBlock ? "await is not allowed in a trailing block (the block is a closure)"
                                      : "await is only allowed inside an async function");
    awaitSeenInFn = true;
    node->operand->accept(this);
    std::string t = getExpressionType(node->operand.get());

    // Strip pointer decorators: the operand should be *Future<T> (or Future<T>*).
    std::string inner = t;
    while (!inner.empty() && inner.front() == '*') inner = inner.substr(1);
    while (!inner.empty() && inner.back()  == '*') inner.pop_back();

    // A `*Future<T>` reaches here in two spellings — source form (`Future<int>`,
    // from a plain function) or already-mangled (`struct:Future_int`, from a
    // template call whose return type went through normalizeType). Normalizing
    // once collapses both to `struct:Future_int` AND registers the reverse map, so
    // a single lookup recovers T regardless of how the future was produced.
    std::string base;
    std::vector<std::string> args;
    std::string norm = normalizeType(inner);
    if (norm.rfind("struct:", 0) == 0) {
        auto ti = templateInstanceArgs.find(norm.substr(7));  // "Future_int"
        if (ti != templateInstanceArgs.end()) { base = ti->second.first; args = ti->second.second; }
    }
    if (base == "Future") {
        std::string res = (args.size() == 1) ? normalizeType(args[0]) : "unknown";
        expressionTypes[node] = res;
        if (!inInstance) node->resolvedType = res;   // consumed by the async transform
        else if (args.size() == 1) node->instanceTypes.push_back({instSubs, args[0]});
    } else {
        if (t != "unknown")
            errorAt(node, "await expects a *Future<T>, got " + t);
        expressionTypes[node] = "unknown";
    }
}

void TypeChecker::visit(FreeClosureExpr* node) {
    node->closure->accept(this);
    std::string t = getExpressionType(node->closure.get());
    if (t != "unknown" && !(t.size() > 3 && t.substr(0, 3) == "fn("))
        errorAt(node, "free_closure expects a closure (fn(...)->R), got " + t);
    expressionTypes[node] = "void";
}

void TypeChecker::visit(ThreadCreateExpr* node) {
    // The thread may run at once and assign any global: a global's narrowing ends here.
    struct GlobalNarrowDrop { TypeChecker* t; ~GlobalNarrowDrop() { t->dropGlobalNarrowings(); } } dropAfter{this};
    node->worker->accept(this);
    std::string t = getExpressionType(node->worker.get());
    if (t != "unknown") {
        ty::Type ft = ty::Type::parse(normalizeType(t));
        bool ok = ft.isFn() && ft.params.empty() && ft.ret && ft.ret->kind == ty::Type::Kind::Void;
        if (!ok) errorAt(node, "thread_create expects a closure 'fn()->void', got '" + t + "'");
    }
    expressionTypes[node] = "*void";
}

// Argument `i` of a call to the generic `fd`. A closure param of the instance being checked,
// passed to a non-escaping param of `fd`, escapes only if that param does; a lambda passed
// to one is decided after every instance is checked.
void TypeChecker::visitGenericCallArg(FunctionDecl* fd, size_t i, const ExprPtr& arg) {
    bool escapes = fd->isAsync || (i < fd->paramEscaping.size() && fd->paramEscaping[i]) ||
                   i >= fd->params.size() || fd->params[i].first == "...";
    auto* id = nodeCast<IdentExpr>(arg.get());
    long own = -1;
    if (id && !escapes && inInstance && curTemplate && nonEscapingFnParams.count(id->name))
        for (size_t j = 0; j < curTemplate->params.size(); ++j)
            if (curTemplate->params[j].second == id->name) own = (long)j;
    if (own >= 0) {
        std::string prev = calleeContext;
        calleeContext = id->name;
        arg->accept(this);
        calleeContext = prev;
        genericParamForwards[{curTemplate, (size_t)own}].insert({fd, i});
    } else {
        arg->accept(this);
    }
    if (auto* lam = nodeCast<LambdaExpr>(arg.get())) {
        auto& d = deferredLambdas[lam];
        if (escapes) d.escapes = true;
        else d.via.insert({fd, i});
    }
}

void TypeChecker::visit(TemplateCallExpr* node) {
    noteDeprecatedUse(node->templateName, node->line, node->col);
    // The callee may assign any global: a global's narrowing ends after the call.
    struct GlobalNarrowDrop { TypeChecker* t; ~GlobalNarrowDrop() { t->dropGlobalNarrowings(); } } dropAfter{this};
    // Variadic access: va_arg<T>(ap) yields the next argument as T.
    if (node->templateName == "va_arg" && node->typeArgs.size() == 1) {
        for (auto& a : node->args) a->accept(this);
        checkVaListArg(node, "va_arg", node->args);
        std::string t = normalizeType(node->typeArgs[0]);
        if (isAggregateValue(t) || isVoidValueType(t)) {
            errorAt(node, "'va_arg' cannot read a '" + node->typeArgs[0] +
                          "': a variadic argument is an integer, a floating-point value or a pointer");
        } else {
            // A variadic argument undergoes the default argument promotions (C): a float
            // arrives as a double and a narrow integer as an int, so reading the narrow
            // type would take the wrong bytes.
            std::string d = tyq::strip(dealiasOperand(t));
            std::string promoted = d == "float" ? "double"
                : (d == "bool" || d == "char" || d == "int8" || d == "int16" || d == "uint8" || d == "uint16") ? "int" : "";
            if (!promoted.empty())
                errorAt(node, "'va_arg' cannot read a '" + node->typeArgs[0] + "': a variadic '" + d +
                              "' is promoted to '" + promoted + "'; read va_arg<" + promoted + "> and convert it");
        }
        expressionTypes[node] = t;
        return;
    }
    // Generic algebraic-variant construction with explicit type args:
    // `Some<int>(5)`, `Left<int,string>(x)`, `None<int>()`.
    auto gv = genericVariants.find(node->templateName);
    if (gv != genericVariants.end()) {
        EnumDecl* ge = genericEnumDecls[gv->second.first];
        std::map<std::string, std::string> subs;
        for (size_t i = 0; i < ge->typeParams.size() && i < node->typeArgs.size(); ++i)
            subs[ge->typeParams[i]] = node->typeArgs[i];
        const auto& payload = ge->payloads[gv->second.second];
        // A generic variant payload takes the payload's instance (`Some(Some(5))` as an
        // `Opt<Opt<int64>>`: the inner one is an `Opt<int64>`).
        for (size_t i = 0; i < node->args.size() && i < payload.size(); ++i)
            inferVariantTarget(node->args[i], substType(payload[i], subs), true);
        for (auto& a : node->args) a->accept(this);
        if (node->args.size() != payload.size())
            errorAt(node, "variant '" + node->templateName + "' expects " +
                std::to_string(payload.size()) + " argument(s), got " + std::to_string(node->args.size()));
        else
            for (size_t i = 0; i < payload.size(); ++i) {
                std::string want = normalizeType(substType(payload[i], subs));
                std::string at = getExpressionType(node->args[i].get());
                if (at != "unknown" && !isValidAssignment(want, at))
                    errorAt(node, "variant '" + node->templateName + "' argument " +
                        std::to_string(i + 1) + " type mismatch");
                else checkVariantLiteral(node, node->templateName, i, want);
            }
        // Build the instance type name (Option<int>) and normalize -> Option_int.
        std::string inst = gv->second.first + "<";
        for (size_t i = 0; i < node->typeArgs.size(); ++i) { if (i) inst += ","; inst += node->typeArgs[i]; }
        inst += ">";
        expressionTypes[node] = normalizeType(inst);
        return;
    }
    auto templ = funcTemplateDecls.find(node->templateName);
    if (templ == funcTemplateDecls.end()) {
        errorAt(node,"undefined template function '" + node->templateName + "'");
        expressionTypes[node] = "unknown";
        return;
    }
    FunctionDecl* fd = templ->second;
    appendDefaultArgs(node->args, node->args.size(), node->templateName, node);
    auto& tp = fd->typeParams;
    size_t errsBefore = errors.size();
    std::map<std::string, std::string> subs;
    for (size_t i = 0; i < tp.size() && i < node->typeArgs.size(); ++i)
        subs[tp[i]] = resolveInstType(node->typeArgs[i]);
    if (node->typeArgs.size() != tp.size())
        errorAt(node, "generic function '" + node->templateName + "' expects " + std::to_string(tp.size()) +
                      " type argument(s), got " + std::to_string(node->typeArgs.size()));
    for (const auto& ta : node->typeArgs) validateStructType(normalizeType(ta), node);
    bool variadic = !fd->params.empty() && fd->params.back().first == "...";
    size_t fixed = variadic ? fd->params.size() - 1 : fd->params.size();
    if (variadic ? node->args.size() < fixed : node->args.size() != fixed)
        errorAt(node, "function '" + node->templateName + "' expects " + (variadic ? "at least " : "") +
                      std::to_string(fixed) + " argument(s), got " + std::to_string(node->args.size()));
    for (size_t i = fixed; i < node->args.size(); ++i) {
        node->args[i]->accept(this);
        checkVariadicArg(node, node->args[i].get(), i);
    }

    checkConstraints(node, fd->constraints, subs);

    // Type-check arguments. A generic variant argument takes the parameter's instance
    // (`get<int64>(Some(8))`), from the type arguments as written.
    std::map<std::string, std::string> writtenSubs;
    for (size_t i = 0; i < tp.size() && i < node->typeArgs.size(); ++i) writtenSubs[tp[i]] = node->typeArgs[i];
    for (size_t i = 0; i < node->args.size() && i < fixed; ++i) {
        if (node->typeArgs.size() == tp.size())
            inferVariantTarget(node->args[i], substType(fd->params[i].first, writtenSubs), true);
        visitGenericCallArg(fd, i, node->args[i]);
        std::string expected = substType(fd->params[i].first, subs);
        std::string got      = getExpressionType(node->args[i].get());
        std::string e = assignabilityError(expected, got, node->args[i].get());
        if (!e.empty())
            errorAt(node,"argument " + std::to_string(i+1) + ": expected " + expected +
                        ", got " + got + " (" + e + ")");
    }

    std::string retType = genericCallRet(fd, subs);
    expressionTypes[node] = retType;
    if (errors.size() == errsBefore && node->typeArgs.size() == tp.size()) {
        std::string mangled = node->templateName;
        for (const auto& t : node->typeArgs) mangled += "_" + mangleTemplate(resolveInstType(t));
        queueInstance(fd, tp, subs, node->templateName, "", mangled, "", fd->sourceFile);
    }
}

void TypeChecker::visit(AllocWithExpr* node) {
    // A call to the allocator's alloc method, which may assign any global.
    struct GlobalNarrowDrop { TypeChecker* t; ~GlobalNarrowDrop() { t->dropGlobalNarrowings(); } } dropAfter{this};
    node->allocator->accept(this);
    node->count->accept(this);
    std::string countType = getExpressionType(node->count.get());
    if (countType != "unknown" && !isIntType(dealiasOperand(countType)))
        errorAt(node,"alloc_with count must be integer, got " + countType);
    // The element type needs a size: a known, non-void type.
    size_t unknownBefore = errors.size();
    validateStructType(normalizeType(node->elemType), node);
    if (errors.size() == unknownBefore && isVoidValueType(node->elemType))
        errorAt(node, "alloc_with of 'void': a void element has no size");
    // The allocator is passed as `self`, so it must be a pointer to the allocator; its
    // type names its alloc method, `*void <Type>_alloc(*<Type> self, int64 size)`.
    std::string at = getExpressionType(node->allocator.get());
    if (at != "unknown") {
        std::string raw = normalizeType(at);
        if (!raw.empty() && raw[0] == '?') raw = raw.substr(1);
        at = raw;
        while (!at.empty() && at.front() == '*') at = at.substr(1);
        while (!at.empty() && at.back()  == '*') at.pop_back();
        if (at.rfind("struct:", 0) == 0) at = at.substr(7);
        bool onePtr = isPointerType(raw) && !isPointerType(getPointeeType(raw));
        auto sig = functionSignatures.find(at + "_alloc");
        if (!onePtr) {
            std::string shown = tyq::strip(getExpressionType(node->allocator.get()));
            for (size_t p; (p = shown.find("struct:")) != std::string::npos;) shown.erase(p, 7);
            errorAt(node, "alloc_with: the allocator must be a pointer to it, got '" + shown +
                          "' (pass its address, as in alloc_with(&a, T, n))");
        } else if (sig == functionSignatures.end()) {
            errorAt(node, "alloc_with: allocator type '" + at + "' has no alloc method (" + at + "_alloc)");
        } else {
            const auto& ps = sig->second.second;
            bool ok = ps.size() == 2 && isPointerType(normalizeType(ps[0])) &&
                      isIntType(dealiasOperand(ps[1])) && isPointerType(normalizeType(sig->second.first));
            if (ok) {
                std::string self = normalizeType(getPointeeType(normalizeType(ps[0])));
                if (self.rfind("struct:", 0) == 0) self = self.substr(7);
                ok = self == at;
            }
            if (!ok)
                errorAt(node, "alloc_with: '" + at + "_alloc' must have the shape '*void " + at + "_alloc(*" + at +
                              " self, int64 size)'");
        }
    }
    expressionTypes[node] = "*" + node->elemType;
}

void TypeChecker::visit(ArrayLitExpr* node) {
    // Untyped: the element type / size come from the target at the declaration
    // site (checked in visit(VarDecl)). Here we just walk the elements.
    for (auto& el : node->elements) el->accept(this);
    expressionTypes[node] = "array-literal";
}

void TypeChecker::visit(StructInitExpr* node) {
    // A template literal (Pair<int,float> { ... }) names an instantiation; run it
    // through normalizeType so the concrete struct gets registered, then resolve.
    std::string sname = node->structName;
    // A literal through a type alias (`type LI = Box<int>; LI{v: 3}`) names the aliased
    // struct; the node is rewritten to it so codegen sees the struct itself (an alias is
    // top-level, so the rewrite is the same in every generic instance).
    for (int hops = 0; hops < 32 && !structs.count(sname); ++hops) {
        auto al = typeAliases.find(sname);
        if (al == typeAliases.end()) break;
        sname = al->second;
        node->structName = sname;
    }
    if (sname.find('<') != std::string::npos) {
        std::string norm = normalizeType(sname);
        if (norm.rfind("struct:", 0) == 0) sname = norm.substr(7);
    }
    auto it = structs.find(sname);
    if (it == structs.end()) {
        std::string base = sname.substr(0, sname.find('<'));
        if (base == sname && templateDecls.count(sname))
            errorAt(node, "generic struct '" + sname + "' needs type arguments, as in " + sname + "<int>{...}");
        else if (base != sname && structs.count(base) && !templateDecls.count(base))
            errorAt(node, "struct '" + base + "' is not generic: write " + base + "{...}");
        else
            errorAt(node,"undefined struct '" + node->structName + "'");
        expressionTypes[node] = "unknown";
        return;
    }

    const auto& fields = it->second.fields;
    bool named = !node->fieldInits.empty() && !node->fieldInits[0].first.empty();
    // A named field is a use of its declaration (references, rename).
    if (inPrimaryFile())
        for (size_t i = 0; i < node->fieldInits.size() && i < node->fieldPositions.size(); ++i) {
            const auto& fname = node->fieldInits[i].first;
            auto fl = fieldLocations.find(node->structName + "." + fname);
            if (!fname.empty() && fl != fieldLocations.end() && node->fieldPositions[i].first > 0)
                useDefs[node->fieldPositions[i]] = {(int)fname.size(), fl->second};
        }
    std::set<std::string> seenFields;
    // A struct two modules define (reported once, at the clash): this literal may have been
    // written for the other declaration, so its shape and field types are not checked.
    bool clashed = clashedNames.count(node->structName) > 0;
    // A literal is either positional or named: mixed, a value would land in the field
    // its position names and could initialize a field a name also sets (`P{1, a: 2}`).
    for (const auto& fi : node->fieldInits)
        if (!clashed && fi.first.empty() == named) {
            errorAt(node, "cannot mix positional and named field initializers in a '" +
                          node->structName + "' literal");
            break;
        }
    // A union's members overlap: its literal initializes exactly one of them (C).
    if (!clashed && it->second.isUnion && node->fieldInits.size() > 1)
        errorAt(node, "a union literal initializes one member ('" + node->structName +
                      "' literal has " + std::to_string(node->fieldInits.size()) + ")");

    for (size_t i = 0; i < node->fieldInits.size(); ++i) {
        // A generic variant field value takes the field's instance (`S{ o: Some(3) }`). The
        // declared field types of a non-generic struct are the same in every instance.
        if (node->structName.find('<') == std::string::npos && !templateDecls.count(node->structName)) {
            std::string ft;
            if (named) { if (const auto* f = it->second.field(node->fieldInits[i].first)) ft = f->type; }
            else if (i < fields.size()) ft = fields[i].type;
            if (!ft.empty()) inferVariantTarget(node->fieldInits[i].second, ft, true);
        }
        const auto& [fname, expr] = node->fieldInits[i];
        expr->accept(this);
        if (clashed) continue;
        ASTNode* at = (expr->line > 0) ? static_cast<ASTNode*>(expr.get()) : node;

        std::string fieldType, shownName;
        if (named) {
            if (const auto* f = it->second.field(fname)) fieldType = f->type;
            shownName = fname;
            if (fieldType.empty())
                errorAt(at, "struct '" + node->structName + "' has no field '" + fname + "'");
            else if (!seenFields.insert(fname).second)
                errorAt(at, "field '" + fname + "' is initialized more than once in '" +
                            node->structName + "' literal");
        } else if (i < fields.size()) {
            fieldType = fields[i].type;
            shownName = fields[i].name;
        } else if (i == fields.size()) {
            errorAt(at, "too many initializers for struct '" + node->structName + "' (it has " +
                        std::to_string(fields.size()) + " field(s), got " +
                        std::to_string(node->fieldInits.size()) + ")");
        }

        if (!fieldType.empty()) {
            std::string valType = getExpressionType(expr.get());
            if (valType == "array-literal") {
                // `{...}` fills an array field (its elements are checked like an array init).
                if (ty::Type::parse(normalizeType(fieldType)).kind != ty::Type::Kind::Array)
                    errorAt(at, "field '" + shownName + "': an array literal '{...}' can only initialize an array type, not '" +
                                fieldType + "'");
            } else if (valType != "unknown") {
                std::string e = dropsConstQual(fieldType, valType)
                    ? "conversion discards a const qualifier ('" + valType + "' to '" + fieldType + "')"
                    : assignabilityError(fieldType, valType, expr.get());
                if (!e.empty()) errorAt(at, "field '" + shownName + "': " + e);
            }
        }
    }

    expressionTypes[node] = "struct:" + sname;
}
