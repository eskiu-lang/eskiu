// typecheck_access.cpp: index, member, cast, literal, identifier and lambda expressions (split from typecheck_expr.cpp).
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

void TypeChecker::visit(IndexExpr* node) {
    node->base->accept(this);
    node->index->accept(this);
    if (node->highIndex) node->highIndex->accept(this);
    checkNullableDeref(node->base.get(), "index");
    // A slice of a fixed array refers to the array's storage (a string or pointer base does not).
    if (node->highIndex && ty::Type::parse(normalizeType(getExpressionType(node->base.get()))).kind ==
                               ty::Type::Kind::Array)
        checkCapturedAddress(node, node->base.get());

    std::string baseType = getExpressionType(node->base.get());
    std::string indexType = getExpressionType(node->index.get());

    if (indexType != "unknown" && !isIntType(indexType)) {
        errorAt(node,"array index must be integer, got " + indexType);
    }
    if (node->highIndex) {
        std::string hiType = getExpressionType(node->highIndex.get());
        if (hiType != "unknown" && !isIntType(hiType))
            errorAt(node, "slice bound must be integer, got " + hiType);
    }

    // Determine the element type of the base (array / slice / pointer / string), through
    // an alias of one (`type PA = *Sq[2]`, an element of type `IS` for `type IS = int[]`).
    std::string shape = dealiasOperand(baseType);
    ty::Type bt = ty::Type::parse(shape);
    std::string elem;
    bool haveElem = false;
    if (shape == "string") { elem = "char"; haveElem = true; }
    else if (bt.kind == ty::Type::Kind::Array || bt.kind == ty::Type::Kind::Slice) {
        elem = bt.elem->str(); haveElem = true;
        // Constant indices and bounds (a literal, an enum member, a `const`, or an
        // expression over them) into a fixed array are checked at compile time. The
        // dimension is a number or an enum member; a `const` dimension is not resolved here.
        long long dimV = -1;
        {
            const std::string& dim = bt.dim;
            if (!dim.empty() && std::all_of(dim.begin(), dim.end(), [](unsigned char c){ return std::isdigit(c); })) {
                try { dimV = std::stoll(dim); } catch (...) { dimV = -1; }
            } else if (auto ec = enumConstants.find(dim); ec != enumConstants.end()) {
                dimV = ec->second;
            }
        }
        const std::string dimS = std::to_string(dimV);
        // A slice lets its elements be written, so a read-only array has no slice.
        if (node->highIndex && bt.kind == ty::Type::Kind::Array) {
            std::string cname;
            if (assignsToConst(node->base.get(), cname))
                errorAt(node, "cannot slice read-only array '" + cname + "': a slice allows writing its elements");
            else if (!isLvalueExpr(node->base.get()))
                errorAt(node, "cannot slice a temporary array: store it in a variable first");
        }
        // Constant slice bounds `a[lo..hi]`: 0 <= lo <= hi on any base, and <= N into a fixed array.
        if (node->highIndex) {
            long long lo = 0, hi = 0;
            bool haveLo = foldConstInt(node->index.get(), lo), haveHi = foldConstInt(node->highIndex.get(), hi);
            if ((haveLo && lo < 0) || (haveHi && hi < 0))
                errorAt(node, "slice bound is negative");
            else if (haveLo && haveHi && lo > hi)
                errorAt(node, "slice bounds out of order: " + std::to_string(lo) + ".." + std::to_string(hi));
            else if (dimV >= 0 && ((haveHi && hi > dimV) || (haveLo && lo > dimV)))
                errorAt(node, "slice bound " + std::to_string(haveHi && hi > dimV ? hi : lo) +
                              " is out of bounds for array of size " + dimS);
        }
        if (!node->highIndex && bt.kind == ty::Type::Kind::Array) {
            long long idx = 0;
            if (foldConstInt(node->index.get(), idx)) {
                if (idx < 0)
                    errorAt(node, "array index " + std::to_string(idx) + " is out of bounds");
                else if (dimV >= 0 && idx >= dimV)
                    errorAt(node, "array index " + std::to_string(idx) +
                                  " is out of bounds for array of size " + dimS);
            }
        }
    } else if (isPointerType(shape)) {
        elem = getPointeeType(shape); haveElem = true;
    }

    // Overloaded subscript: `base[i]` on a non-built-in indexable resolves to a user
    // `operator [](Base, Index)` (read/rvalue form; a slice `base[lo..hi]` is not overloaded).
    if (!haveElem && !node->highIndex) {
        std::string ret, opFn = resolveOperator("[]", {baseType, indexType}, ret);
        if (!opFn.empty()) {
            if (!inInstance) node->opFunc = opFn;
            operatorCallNodes.insert(node);
            dropGlobalNarrowings();
            calledFns.insert(opFn);
            expressionTypes[node] = ret;
            return;
        }
    }

    if (!haveElem) {
        // Only a base certain to be unindexable is reported: a number, or a struct/sum-type
        // value with no `operator []` (other spellings, e.g. an array of fn values, are
        // left to the element resolution in codegen).
        std::string nb = normalizeType(tyq::strip(baseType));
        if (isNumericType(nb) || nb.rfind("struct:", 0) == 0 || adtEnums.count(nb) || nb == "null")
            errorAt(node, "cannot index into a value of type '" + ty::Type::parse(baseType).nominalName() + "'");
        expressionTypes[node] = "unknown";
        return;
    }

    // `base[lo..hi]` yields a slice of the element type; `base[i]` yields the element.
    expressionTypes[node] = node->highIndex ? (elem + "[]") : elem;
}

void TypeChecker::visit(MemberExpr* node) {
    node->base->accept(this);
    checkNullableDeref(node->base.get(), "access a member of");

    std::string baseType = getExpressionType(node->base.get());
    if (!baseType.empty() && baseType[0] == '?') baseType = baseType.substr(1);   // `?*T` derefs like `*T`
    if (baseType.rfind("const ", 0) == 0) baseType = baseType.substr(6);         // `const *T` derefs like `*T`
    // Through an alias (`type PSq = *Sq`, `type IS = int[]`) the member is the target's.
    baseType = dealiasOperand(baseType);
    if (!baseType.empty() && baseType[0] == '?') baseType = baseType.substr(1);
    if (node->member == kCompleteSentinel) {           // --complete-at after `recv.`
        if (!completionDone) { completeMembers(baseType); completionDone = true; }
        expressionTypes[node] = "unknown";
        return;
    }

    // Slice `.len` → int64 (the fat pointer's length field).
    if (node->member == "len" && ty::Type::parse(baseType).kind == ty::Type::Kind::Slice) {
        expressionTypes[node] = "int64";
        return;
    }

    // Auto-deref pointer-to-struct: *Point, struct:Point*, Point* → struct:Point
    if (hasPointerSuffix(baseType)) {
        baseType = extractBaseType(baseType);
    } else if (!baseType.empty() && baseType.front() == '*') {
        baseType = baseType.substr(1); // strip leading *
    }
    baseType = normalizeType(baseType); // "Point" → "struct:Point" if registered

    // Check if base is a struct type
    if (baseType.find("struct:") == 0) {
        // Extract struct name (remove "struct:" prefix)
        std::string structName = baseType.substr(7);  // strlen("struct:") = 7

        // Look up struct in registry
        auto it = structs.find(structName);
        if (it == structs.end()) {
            errorAt(node, "undefined struct '" + structName + "'");
            expressionTypes[node] = "unknown";
            return;
        }

        // Look for the member in struct's fields
        const auto& structInfo = it->second;
        if (const StructDecl::Field* fp = structInfo.field(node->member)) {
            const StructDecl::Field& field = *fp;
            {
                std::string owner = structName;
                if (auto ti = templateInstanceArgs.find(structName); ti != templateInstanceArgs.end())
                    owner = ti->second.first;
                auto fl = fieldLocations.find(owner + "." + field.name);
                recordMemberTooling(node, field.type, "",
                                    fl != fieldLocations.end() && fl->second.line > 0 ? &fl->second : nullptr);
                // Found the member, return its type (a narrow bitfield reads as `int`, C)
                expressionTypes[node] = field.type;
                if (field.bitWidth > 0 &&
                    tyq::bitfieldReadType(dealiasOperand(tyq::strip(field.type)), field.bitWidth) == "int")
                    expressionTypes[node] = "int";
                return;
            }
        }

        // Member not found in struct
        errorAt(node,"struct '" + structName + "' has no member '" + node->member + "'" +
                     didYouMean(node->member, 'm', baseType));
        expressionTypes[node] = "unknown";
    } else if (baseType == "unknown") {
        // Base type is unknown, can't validate member access
        expressionTypes[node] = "unknown";
    } else {
        // Base is not a struct
        errorAt(node,"cannot access member '" + node->member + "' on non-struct type '" + baseType + "'");
        expressionTypes[node] = "unknown";
    }
}

void TypeChecker::visit(CastExpr* node) {
    node->expr->accept(this);
    // `(void)e` (C): evaluate `e` and discard its value, e.g. to mark a result as unused.
    if (tyq::strip(node->targetType) == "void") {
        expressionTypes[node] = "void";
        return;
    }
    // Validate that struct types exist in casts
    std::string normalizedType = normalizeType(node->targetType);
    validateStructType(normalizedType, node);
    // A cast converts between scalars: numbers (incl. bool/char/enums), pointers, and
    // integer<->pointer. An aggregate (struct, union, sum type, array, slice, closure)
    // casts only to its own type; float<->pointer has no meaning.
    {
        std::string from = normalizeType(tyq::strip(getExpressionType(node->expr.get())));
        std::string to = normalizeType(tyq::strip(normalizedType));
        if (!from.empty() && from[0] == '?') from = from.substr(1);
        if (!to.empty() && to[0] == '?') to = to.substr(1);
        bool fnName = false;   // a top-level function cast to a pointer is its raw C address
        if (auto* id = nodeCast<IdentExpr>(node->expr.get()))
            fnName = lookupSymbol(id->name).empty() && functionSignatures.count(id->name);
        auto isPtr = [&](const std::string& t) { return isPointerType(t) || t == "null"; };
        bool ok = from == "unknown" || to == "unknown" || from == to || (fnName && isPtr(to)) ||
                  (isNumericType(from) && isNumericType(to)) ||
                  (isPtr(from) && isPtr(to)) ||
                  (isIntType(from) && isPtr(to)) || (isPtr(from) && isIntType(to));
        if (!ok)
            errorAt(node, "cannot cast '" + getExpressionType(node->expr.get()) + "' to '" + node->targetType + "'");
        // A floating constant cast to an integer type must fit it: the conversion is
        // undefined in C, and the backends would not agree on a value.
        const Expr* src = node->expr.get();
        bool neg = false;
        if (auto* u = nodeCast<const UnaryExpr>(src); u && u->op == "-") { neg = true; src = u->operand.get(); }
        auto* lit = nodeCast<const LiteralExpr>(src);
        bool isInt = true; long long iv = 0; double v = 0;
        if (ok && isIntType(to) && to != "bool" && foldConstNum(node->expr.get(), isInt, iv, v) &&
            !isInt && !floatConstFitsInt(v, to)) {
            if (lit && lit->kind == LiteralExpr::Kind::FLOAT)
                errorAt(node, "floating constant " + std::string(neg ? "-" : "") + lit->value +
                              " is out of range for '" + node->targetType + "'");
            else {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%g", v);
                errorAt(node, "floating constant expression (value " + std::string(buf) +
                              ") is out of range for '" + node->targetType + "'");
            }
        }
    }
    expressionTypes[node] = normalizedType;
}

void TypeChecker::visit(LiteralExpr* node) {
    switch (node->kind) {
        case LiteralExpr::Kind::INT:
            // C: an unsuffixed literal is the first of int, int64 that holds it (codegen
            // emits the value in that width); only an unsigned 64-bit type holds more.
            try {
                long long v = std::stoll(node->value, nullptr, 0);
                expressionTypes[node] = (v >= INT32_MIN && v <= INT32_MAX) ? "int" : "int64";
            } catch (...) {
                expressionTypes[node] = "uint64";
            }
            break;
        case LiteralExpr::Kind::FLOAT:
            // A float literal lowers to a `double` constant (the lexer has no
            // float/double distinction; codegen emits ConstantFP::getDoubleTy).
            // Sema previously said "float" — a latent disagreement with codegen.
            expressionTypes[node] = "double";
            break;
        case LiteralExpr::Kind::STRING:
            expressionTypes[node] = "string";
            break;
        case LiteralExpr::Kind::CHAR:
            expressionTypes[node] = "char";
            break;
        case LiteralExpr::Kind::BOOL:
            expressionTypes[node] = "bool";
            break;
        case LiteralExpr::Kind::NULL_VAL:
            expressionTypes[node] = "null";
            break;
        default:
            expressionTypes[node] = "unknown";
    }
}

void TypeChecker::visit(IdentExpr* node) {
    if (node->name == kCompleteSentinel) {             // --complete-at
        if (!completionDone) { completeScope(); completionDone = true; }
        expressionTypes[node] = "unknown";
        return;
    }
    // -Wall: a function referenced as a value counts as used.
    if (functionSignatures.count(node->name)) {
        calledFns.insert(node->name);
        noteDeprecatedUse(node->name, node->line, node->col);
    }

    // Escape soundness: a watched closure param referenced anywhere other than
    // as the immediate callee of a call escapes (see visit(FunctionDecl)).
    if (nonEscapingFnParams.count(node->name) && node->name != calleeContext)
        escapedFnParams.insert(node->name);
    if (lambdaLocals.count(node->name) && node->name != calleeContext)
        escapedLambdaLocals.insert(node->name);

    std::string type = lookupSymbol(node->name);
    if (!type.empty() && !scriptGlobalPos.empty() && scopeOf(node->name) == 0) {
        // A script's statement before the declaration of one of its globals.
        auto sp = scriptGlobalPos.find(node->name);
        if (sp != scriptGlobalPos.end() && sp->second > std::make_pair(node->line, node->col)) type.clear();
    }
    if (type.empty() && enumConstants.count(node->name)) {
        // Bare enum member, e.g. `Red` — an int constant.
        expressionTypes[node] = "int";
        if (inPrimaryFile()) useLocations[{node->line, node->col}] = node->name;
        return;
    }
    if (type.empty() && adtVariants.count(node->name)) {
        // Bare algebraic variant, e.g. `None` — constructs the enum value. Variants
        // with a payload must be called (`Some(x)`), handled in visit(CallExpr).
        auto& info = adtVariants[node->name];
        const auto& payload = enumDecls[info.first]->payloads[info.second];
        if (!payload.empty())
            errorAt(node, "variant '" + node->name + "' needs " +
                std::to_string(payload.size()) + " argument(s)");
        expressionTypes[node] = info.first;     // the enum value type
        return;
    }
    // A top-level function used as a value decays to a `fn(params)->ret` pointer.
    if (type.empty() && functionSignatures.count(node->name)) {
        const auto& sig = functionSignatures[node->name];   // (returnType, paramTypes)
        std::string t = "fn(";
        for (size_t i = 0; i < sig.second.size(); ++i) {
            if (i) t += ",";
            t += sig.second[i];
        }
        t += ")->" + sig.first;
        expressionTypes[node] = t;
        if (inPrimaryFile()) useLocations[{node->line, node->col}] = node->name;
        return;
    }
    if (type.empty() && genericVariants.count(node->name)) {
        // A generic enum's variant does not name its type arguments by itself (they are
        // not inferred from the target type): `None<int>()`.
        errorAt(node, "variant '" + node->name + "' of generic enum '" + genericVariants[node->name].first +
                      "' needs explicit type arguments, as in " + node->name + "<int>()");
        expressionTypes[node] = "unknown";
    } else if (type.empty()) {
        errorAt(node,"undefined variable '" + node->name + "'" + didYouMean(node->name, 'v'));
        expressionTypes[node] = "unknown";
    } else if (type[0] == '?' && narrowedNonNull.count(narrowKey(node->name))) {
        expressionTypes[node] = type.substr(1);   // proven non-null here: a plain `*T`
    } else {
        expressionTypes[node] = type;
    }
    // Record use-site so go-to-definition can map cursor → definition: a local or
    // parameter maps to the exact symbol found; a global maps by name.
    if (inPrimaryFile()) {
        const Symbol* sym = findSymbol(node->name);
        auto global = scopes.front().find(node->name);
        bool isGlobal = global != scopes.front().end() && &global->second == sym;
        if (sym && !isGlobal && sym->line > 0)
            useDefs[{node->line, node->col}] = {(int)node->name.size(), {sym->line, sym->col, sym->file}};
        else
            useLocations[{node->line, node->col}] = node->name;
    }

    // Capture detection: inside a lambda, a name is captured when it resolves to
    // a variable in an enclosing scope (below the lambda's own scopes). We key off
    // the variable's defining scope index, NOT functionSignatures — a param or
    // local that shadows a same-named top-level function must still be captured.
    if (!captureStack.empty() && !type.empty()) {
        int defIdx = scopeOf(node->name);
        // Capture only enclosing-function scopes: index >= 1 (the global scope
        // at 0 is module-level and accessed directly, not captured by value).
        // A variable must be captured by EVERY enclosing lambda it is outer to,
        // not just the innermost one, so a nested lambda's use is threaded
        // through each intervening lambda's environment (transitive capture).
        // Without this, an inner lambda that references a variable two scopes up
        // would read the enclosing lambda's non-captured value and miscompile
        // ("Referring to an instruction in another function").
        // A `static` local has static storage: like a global, the lambda body refers
        // to the one cell directly instead of capturing a copy.
        if (defIdx >= 1 && !scopes[defIdx][node->name].isStatic) {
            for (size_t k = 0; k < captureStack.size(); ++k) {
                if (defIdx < captureBoundary[k])
                    captureStack[k][node->name] = type;
            }
        }
    }
}

void TypeChecker::visit(LambdaExpr* node) {
    // Build fn(T,U)->R type string
    std::string sig = "fn(";
    for (size_t i = 0; i < node->params.size(); ++i) {
        if (i > 0) sig += ",";
        sig += node->params[i].first;
    }
    sig += ")->" + node->returnType;

    // Push a capture collector before entering the lambda scope.
    // visit(IdentExpr) will add outer-scope vars to captureStack.back().
    captureStack.push_back({});
    captureBoundary.push_back((int)scopes.size());   // scopes below this are "outer"

    pushScope();
    std::string savedReturn = currentFunctionReturnType, savedRaw = currentRawReturnType;
    currentFunctionReturnType = node->returnType;
    currentRawReturnType = node->returnType;
    // The body is its own function: a break/continue there cannot target a loop (or
    // switch) of the enclosing function.
    std::vector<std::string> savedLoops = std::move(loopLabelStack);
    loopLabelStack.clear();
    int savedSwitch = switchDepth;
    switchDepth = 0;
    // A lambda is its own (non-async) function: an `await` in its body does not belong to
    // an enclosing async function.
    bool savedAsync = inAsyncFn, savedAwait = awaitSeenInFn, savedVariadic = inVariadicFn;
    bool savedTrailing = inTrailingBlock;
    inTrailingBlock = node->trailing;
    int savedFinally = finallyDepth, savedCatch = catchDepth;
    inAsyncFn = false;
    finallyDepth = 0;
    catchDepth = 0;
    inVariadicFn = false;
    // Captures are by value: an assignment to a captured name inside the body changes the
    // lambda's copy, so it must not end a narrowing of the enclosing variable.
    std::set<std::string> savedNarrowed = narrowedNonNull;
    // The body runs later, when a global (or `static` local) may have been nulled.
    for (auto it = narrowedNonNull.begin(); it != narrowedNonNull.end();) {
        size_t at = it->rfind('@');
        if (at != std::string::npos && it->compare(at, std::string::npos, "@0") == 0) it = narrowedNonNull.erase(it);
        else ++it;
    }
    for (const auto& p : node->params) {
        if (scopes.back().count(p.second)) errorAt(node, "duplicate parameter '" + p.second + "' in lambda");
        defineSymbol(p.second, normalizeType(p.first), node->line, node->col, /*isParam=*/true);
        if (tyq::bindingConst(p.first)) scopes.back()[p.second].isConst = true;
    }
    // Mark param names so IdentExpr doesn't treat them as captures
    std::set<std::string> paramNames;
    for (const auto& p : node->params) paramNames.insert(p.second);

    if (node->body) node->body->accept(this);
    // A lambda returning a value must return on every path, like a function.
    if (node->body && tyq::strip(node->returnType) != "void" && stmtCanCompleteNormally(node->body.get()))
        errorAt(node, "missing return in lambda returning '" + node->returnType +
                      "' (control can reach the end without returning a value)");
    currentFunctionReturnType = savedReturn;
    currentRawReturnType = savedRaw;
    loopLabelStack = std::move(savedLoops);
    switchDepth = savedSwitch;
    inAsyncFn = savedAsync;
    finallyDepth = savedFinally;
    catchDepth = savedCatch;
    inVariadicFn = savedVariadic;
    // An `await` in a trailing block is reported there; the function does not also lack one.
    awaitSeenInFn = savedAwait || (node->trailing && awaitSeenInFn);
    inTrailingBlock = savedTrailing;
    narrowedNonNull = savedNarrowed;
    popScope();

    // Harvest captures: only outer-scope vars, not params. A generic body's lambdas
    // keep the source-form captures TemplateCapturePass recorded (shared by instances).
    if (!inInstance) {
        node->captures.clear();
        for (const auto& [name, type] : captureStack.back()) {
            if (!paramNames.count(name)) {
                node->captures.push_back({name, type});
                if (nonEscapingFnParams.count(name)) watchedCaptures.push_back({node, name});
                if (lambdaLocals.count(name)) localCaptures.push_back({node, name});
            }
        }
    }
    captureStack.pop_back();
    captureBoundary.pop_back();

    expressionTypes[node] = sig;
}
