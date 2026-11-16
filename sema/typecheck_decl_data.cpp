// typecheck_decl_data.cpp: variables, constant initializers and the struct, extern, enum, alias, interface and union declarations (split from typecheck_decl.cpp).
#include "type_checker.h"
#include <cstdint>
#include <functional>
#include <algorithm>
#include <set>

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with codegen; see template_utils.h.
#include "../template_utils.h"
#include "../intrinsics.h"
#include "../ast/ast_walk.h"
#include "../ast/type_qual.h"
#include "template_capture_pass.h"

// Is `e` a compile-time constant initializer codegen can fold? A literal, `sizeof`, an
// enum member, a top-level `const`, the address of a global, a non-capturing lambda, and
// unary/binary/ternary/cast combinations of those, or an array/struct literal built from
// constants. A call or a read of a non-const variable is not constant (as in C), since
// it would need code to run before `main`.
bool TypeChecker::isConstInit(const ExprPtr& e) const {
    if (!e) return true;
    if (nodeCast<LiteralExpr>(e.get())) return true;
    if (nodeCast<SizeofExpr>(e.get())) return true;
    if (auto* lam = nodeCast<LambdaExpr>(e.get())) return lam->captures.empty();
    if (auto* id = nodeCast<IdentExpr>(e.get())) {
        if (enumConstants.count(id->name)) return true;
        // Only a global `const` (scopes[0]) folds; a local const may be a runtime value.
        int si = scopeOf(id->name);
        // A top-level function name decays to a constant closure (`Op g = add;`).
        if (si < 0) return functionSignatures.count(id->name) > 0;
        return si == 0 && scopes[0].find(id->name)->second.isConst;
    }
    if (auto* u = nodeCast<UnaryExpr>(e.get())) {
        if (u->op == "&") {
            // The address of a global variable is a link-time constant.
            auto* id = nodeCast<IdentExpr>(u->operand.get());
            if (!id || functionSignatures.count(id->name)) return false;
            return scopeOf(id->name) == 0;
        }
        return (u->op == "-" || u->op == "~" || u->op == "!" || u->op == "+") && isConstInit(u->operand);
    }
    if (auto* b = nodeCast<BinaryExpr>(e.get())) {
        static const std::set<std::string> ops = {"+","-","*","/","%","&","|","^","<<",">>",
            "==","!=","<",">","<=",">=","&&","||"};
        // Down the left spine with a loop (a long `A + B + C ...` is as deep as it is long).
        for (;;) {
            if (!ops.count(b->op) || b->opFunc.size() || !isConstInit(b->right)) return false;
            auto* l = nodeCast<BinaryExpr>(b->left.get());
            if (!l) return isConstInit(b->left);
            b = l;
        }
    }
    if (auto* t = nodeCast<TernaryExpr>(e.get()))
        return isConstInit(t->condition) && isConstInit(t->thenExpr) && isConstInit(t->elseExpr);
    if (auto* c = nodeCast<CastExpr>(e.get())) return isConstInit(c->expr);
    if (auto* a = nodeCast<ArrayLitExpr>(e.get())) {
        for (auto& el : a->elements) if (!isConstInit(el)) return false;
        return true;
    }
    if (auto* s = nodeCast<StructInitExpr>(e.get())) {
        for (auto& fi : s->fieldInits) if (!isConstInit(fi.second)) return false;
        return true;
    }
    return false;
}

// The type a bare integer-literal range bound takes: the first of int, int64, uint64
// that holds its value (C's rule for an unsuffixed literal, extended to uint64).
static std::string rangeLiteralType(const std::string& v) {
    bool neg = !v.empty() && v[0] == '-';
    unsigned long long mag = 0;
    try { mag = std::stoull(neg ? v.substr(1) : v, nullptr, 0); }
    catch (...) { return "int"; }
    if (mag <= (neg ? 2147483648ULL : 2147483647ULL)) return "int";
    if (neg || mag <= 9223372036854775807ULL) return "int64";
    return "uint64";
}

// Checks a (possibly nested) array literal against a (possibly
// multi-dimensional) array type: `int[2][3]` expects two rows, each itself a
// `{...}` of up to three ints; a short list zero-fills. Through a type alias
// (`type A3 = int[3]; A3 a = {...}`) the target is the array, at any level.
// Errors are reported at `at`.
void TypeChecker::checkArrayLiteral(const std::string& arrT0, ArrayLitExpr* a, ASTNode* at) {
    auto dealiasArr = [&](std::string t) {
        for (int hops = 0; hops < 32; ++hops) {
            auto al = typeAliases.find(tyq::strip(t));
            if (al == typeAliases.end()) break;
            t = al->second;
        }
        return t;
    };
    std::string arrT = dealiasArr(arrT0);
    ty::Type ty = ty::Type::parse(arrT);
    if (ty.kind != ty::Type::Kind::Array) {
        errorAt(at, "an array literal '{...}' can only initialize an array type, not '" + arrT + "'");
        return;
    }
    std::string elemT = ty.elem->str();
    const std::string& dim = ty.dim;
    bool dimNum = !dim.empty() &&
        std::all_of(dim.begin(), dim.end(), [](unsigned char c){ return std::isdigit(c); });
    if (dimNum && a->elements.size() > (size_t)std::stoull(dim))
        errorAt(at, "array literal has " + std::to_string(a->elements.size()) +
                    " elements but '" + arrT + "' holds " + dim);
    bool elemIsArray = ty::Type::parse(dealiasArr(elemT)).kind == ty::Type::Kind::Array;
    for (auto& el : a->elements) {
        if (elemIsArray) {
            if (auto* sub = nodeCast<ArrayLitExpr>(el.get()))
                checkArrayLiteral(elemT, sub, at);
            else
                errorAt(at, "nested array initializer expected '{...}' for element type '" + elemT + "'");
        } else {
            std::string et = getExpressionType(el.get());
            std::string e = assignabilityError(elemT, et, el.get());
            if (!e.empty()) errorAt(at, "array element: " + e);
        }
    }
}

void TypeChecker::visit(VarDecl* node) {
    // `extern <type> <name>;` names a variable defined in another translation unit
    // (a C global). It lives at top level and carries no initializer.
    ASTNode* savedCtx = posCtx;
    if (node->line > 0) posCtx = node;
    struct CtxRestore { ASTNode*& p; ASTNode* v; ~CtxRestore() { p = v; } } ctxRestore{posCtx, savedCtx};
    if (node->isExtern) {
        if (scopes.size() > 1)
            errorAt(node, "'extern' is only allowed on a top-level variable");
        if (node->initializer)
            errorAt(node, "an 'extern' variable cannot have an initializer");
    }

    // `static` is a local-only qualifier whose initializer must be a compile-time
    // constant (it runs once, at load time), as in C. Accept every form codegen's
    // constant folder can emit — a literal, a numeric unary/cast of one, or an
    // array/struct literal (or sizeof) built from those — not just a bare literal.
    if (node->isStatic) {
        if (scopes.size() <= 1)
            errorAt(node, "'static' is only allowed on a local variable");
        if (node->initializer && !isConstInit(node->initializer))
            errorAt(node, "a 'static' local's initializer must be a constant");
    }

    // Record definition location
    if (node->line > 0 && !inInstance)
        definitionLocations[node->name] = {node->line, node->col, diagFile()};
    // A `fn(...)` local declared without a value can still be a lambda local: lambdas
    // assigned to it later (`f = <lambda>`) are bound to it.
    if (!node->initializer && scopes.size() > 1 && !node->isStatic && !inInstance &&
        ty::Type::parse(node->type).isFn())
        lambdaLocals.insert(node->name);
    if (node->initializer) {
        // Reconcile a lambda initializer's return type with a declared fn(...)->R
        // target BEFORE checking its body. A mismatched lambda header (e.g. an
        // `int(int x)` used as `fn(int)->float`) would otherwise emit a function
        // whose return type disagrees with the closure's call ABI — correct at -O0
        // by luck, a silent miscompile (0.0) under -O2. Setting the lambda's return
        // type to R lets its `return` coerce to R through the normal path.
        if (auto* lam = nodeCast<LambdaExpr>(node->initializer.get())) {
            ty::Type dt = ty::Type::parse(node->type);
            if (dt.isFn() && dt.ret && dt.ret->str() != lam->returnType)
                lam->returnType = dt.ret->str();
        }
        hintIfaceTarget(node->initializer.get(), node->type);
        inferVariantTarget(node->initializer, node->type, true);
        node->initializer->accept(this);
        // A lambda bound to a local that is only ever called does not outlive the call.
        if (auto* lam = nodeCast<LambdaExpr>(node->initializer.get());
            lam && scopes.size() > 1 && !node->isStatic && !inInstance) {
            lambdaLocal[lam] = node->name;
            lambdaLocals.insert(node->name);
            if (inAsyncFn) asyncLocalLambdas.insert(lam);
        }
        // A `for (i in A..B)` bound decl takes its bound's integer type (promoted to at
        // least `int`); visit(ForStmt) then widens both decls to their common type.
        bool badRangeBound = false;
        if (node->rangeBound) {
            std::string bt = getExpressionType(node->initializer.get());
            if (auto* lit = nodeCast<LiteralExpr>(node->initializer.get()))
                if (lit->kind == LiteralExpr::Kind::INT) bt = rangeLiteralType(lit->value);
            if (bt != "unknown") {
                std::string nt = tyq::strip(normalizeType(tyq::strip(bt)));
                std::string rt = ty::rangeVarType(nt, "int");
                if (rt.empty()) {
                    errorAt(node->initializer.get(), "range bound must be an integer, got '" + nt + "'");
                    badRangeBound = true;
                } else {
                    node->type = rt;
                }
            }
        }
        // A global's initializer is evaluated at compile time (there is no code before
        // `main` to run it), so it must be a constant expression, as in C.
        if (scopes.size() <= 1 && !node->isStatic && !node->scriptInit && !isConstInit(node->initializer))
            errorAt(node, "initializer of global '" + node->name + "' is not a compile-time constant");
        // Array literal `= {..}`: the target must be a fixed-size array; check the
        // element count (fewer than the size zero-fill, C-style) and element types.
        if (auto* arr = nodeCast<ArrayLitExpr>(node->initializer.get())) {
            // Recursively check a (possibly nested) array literal against a
            // (possibly multi-dimensional) array type: `int[2][3]` expects two rows,
            // each itself a `{...}` of up to three ints. A short list zero-fills.
            // Through a type alias (`type A3 = int[3]; A3 a = {...}`) the target is the array,
            // at any level (`A3[2] m = {{..}, {..}}`).
            checkArrayLiteral(node->type, arr, node);
        } else if (!badRangeBound) {
            std::string initType = getExpressionType(node->initializer.get());
            if (initType != "unknown") {
                if (dropsConstQual(node->type, initType))
                    errorAt(node, "cannot initialize '" + node->type + "' from '" + initType +
                                  "': conversion discards a const qualifier");
                else {
                    std::string e = assignabilityError(node->type, initType, node->initializer.get());
                    if (!e.empty()) errorAt(node, "initializing '" + node->name + "': " + e);
                }
            }
        }
    }
    // A const must be initialized — there is no later point to assign it. An
    // `extern const` is the exception: its definition (and value) lives elsewhere.
    if (node->isConst && !node->initializer && !node->isExtern) {
        errorAt(node, "const '" + node->name + "' must be initialized");
    }

    // Normalize the type (e.g., "Point" -> "struct:Point")
    std::string normalizedType = normalizeType(node->type);
    // A classic (payload-less) enum collapses to `int` in normalizeType, but keep its
    // nominal name on the variable so `match` can recover the variant set. It still behaves
    // as an int everywhere else (every other check runs the type back through normalizeType).
    if (normalizedType == "int" && plainEnumDecls.count(tyq::strip(dealiasOperand(node->type))))
        normalizedType = tyq::strip(dealiasOperand(node->type));

    // Validate that struct types exist before use
    validateStructType(normalizedType, node);
    if (std::string vm = voidTypeError(node->type); !vm.empty())
        errorAt(node, "variable '" + node->name + "' " + vm);

    // Preserve a pointee-const qualifier through normalization so the symbol
    // remembers it's read-only (const checks read it back; everything else strips).
    std::string storedType = normalizedType;
    if (tyq::baseConst(node->type) && tyq::isPtr(node->type))
        storedType = "const " + normalizedType;

    // Two locals of the same name in one scope (`let x; let x;`). Globals are checked
    // once for the whole program (checkTopLevelNames); an inner block may shadow.
    // A function's (or lambda's) outermost block shares the parameters' scope, as in C,
    // so a local there may not reuse a parameter's name.
    auto redeclaresParam = [&]() {
        if (scopes.size() < 2) return false;
        auto& outer = scopes[scopes.size() - 2];
        auto it = outer.find(node->name);
        return it != outer.end() && it->second.isParam;
    };
    if (scopes.size() > 1 && (scopes.back().count(node->name) || redeclaresParam()))
        errorAt(node, "redefinition of '" + node->name + "' in the same scope");
    defineSymbol(node->name, storedType, node->line, node->col, /*isParam=*/false);
    if (node->isConst && !scopes.empty()) {
        scopes.back()[node->name].isConst = true;
        scopes.back()[node->name].constInit = node->initializer.get();
    }
    if (node->isStatic && !scopes.empty()) scopes.back()[node->name].isStatic = true;
    if (scopes.size() > 1 && !node->isStatic && !node->isExtern) {
        localAddrHolders.erase(localAddrKey(node->name));
        if (node->initializer) trackLocalAddr(node->name, node->initializer.get());
        // A local this large overflows a thread's stack (an async function's locals
        // live in its heap frame).
        unsigned long long size = 0, align = 0;
        if (!inAsyncFn && constLayout(normalizedType, size, align) && size > kLargeLocalBytes)
            noteWarning("frame-larger-than", node->line, node->col,
                        "local '" + node->name + "' takes " + std::to_string(size) +
                        " bytes of stack; a thread's stack is 512 KiB to 8 MiB, so allocate it with alloc<T> or make it static");
    }
}

void TypeChecker::visit(StructDecl* node) {
    defineSymbol(node->name, "struct:" + node->name);
    checkTypeParams(node, node->name, node->typeParams);
    // A field default (`int t = 30;`) is a constant of the field's type, as a global's
    // initializer; it is what an omitted or unset field of the struct holds.
    for (auto& f : node->fields) {
        if (!f.defaultValue) continue;
        Expr* e = f.defaultValue.get();
        ASTNode* at = e->line > 0 ? static_cast<ASTNode*>(e) : node;
        if (!node->typeParams.empty()) {
            errorAt(at, "field '" + f.name + "' of generic struct '" + node->name + "' cannot have a default value");
            continue;
        }
        if (f.bitWidth != 0) { errorAt(at, "bitfield '" + f.name + "' cannot have a default value"); continue; }
        inferVariantTarget(f.defaultValue, f.type, true);
        f.defaultValue->accept(this);
        if (!isConstInit(f.defaultValue)) {
            errorAt(at, "default value of field '" + f.name + "' is not a compile-time constant");
            continue;
        }
        if (auto* arr = nodeCast<ArrayLitExpr>(e)) {
            checkArrayLiteral(f.type, arr, at);
            continue;
        }
        std::string err = assignabilityError(f.type, getExpressionType(e), e);
        if (!err.empty())
            errorAt(at, "default value of field '" + f.name + "': " + err);
    }
    {
        std::set<std::string> names;
        for (const auto& f : node->fields)
            if (!names.insert(f.name).second)
                errorAt(node, "duplicate field '" + f.name + "' in struct '" + node->name + "'");
        std::set<std::string> methods;
        for (const auto& m : node->methods)
            if (auto* mf = nodeCast<FunctionDecl>(m.get())) {
                if (names.count(mf->name))
                    errorAt(mf, "method '" + mf->name + "' of struct '" + node->name + "' has the same name as a field");
                if (!methods.insert(mf->name).second)
                    errorAt(mf, "duplicate method '" + mf->name + "' in struct '" + node->name + "'");
                for (const auto& p : mf->params)
                    if (p.second == "self")
                        errorAt(mf, "method '" + mf->name + "' of struct '" + node->name +
                                    "' cannot have a parameter named 'self': it is the implicit receiver");
            }
    }
    checkUnnamedBitfields(node, node->name, node->pads);
    // Field types must name known types (a template's fields mention its type params and
    // are checked per instantiation through the instance's type arguments instead).
    if (node->typeParams.empty()) {
        for (const auto& f : node->fields) {
            validateStructType(normalizeType(f.type), node);
            if (std::string vm = voidTypeError(f.type); !vm.empty())
                errorAt(node, "field '" + f.name + "' of '" + node->name + "' " + vm);
            checkBitfield(node, node->name, f);
        }
        for (const auto& method : node->methods)
            if (auto func = nodeCast<FunctionDecl>(method.get())) {
                for (const auto& p : func->params) {
                    validateStructType(normalizeType(p.first), func);
                    if (std::string vm = voidTypeError(p.first); !vm.empty())
                        errorAt(func, "parameter '" + p.second + "' " + vm);
                }
                validateStructType(normalizeType(func->returnType), func);
            }
    }
    // A generic struct's methods mention its type params: each body is checked per
    // struct instance (queued when `Box<int>` is instantiated). Lambda captures are
    // resolved here, type-independently, as for a generic function.
    if (!node->typeParams.empty()) {
        std::string selfT = "*" + node->name + "<";
        for (size_t i = 0; i < node->typeParams.size(); ++i) selfT += (i ? "," : "") + node->typeParams[i];
        selfT += ">";
        for (const auto& method : node->methods)
            if (auto* func = nodeCast<FunctionDecl>(method.get()); func && func->body) {
                TemplateCapturePass p;
                p.run(func, selfT);
            }
        return;
    }
    // Type-check method bodies
    for (const auto& method : node->methods) {
        if (auto func = nodeCast<FunctionDecl>(method.get())) {
            std::string savedReturn = currentFunctionReturnType, savedRaw = currentRawReturnType;
            currentFunctionReturnType = func->returnType;
            currentRawReturnType = func->returnType;
            bool savedVariadic = inVariadicFn;
            inVariadicFn = !func->params.empty() && func->params.back().first == "...";
            pushScope();
            defineSymbol("self", "*" + node->name);
            for (const auto& p : func->params) {
                defineSymbol(p.second, normalizeType(p.first));
                if (tyq::bindingConst(p.first)) scopes.back()[p.second].isConst = true;
            }
            if (func->body) func->body->accept(this);
            popScope();
            currentFunctionReturnType = savedReturn;
            currentRawReturnType = savedRaw;
            inVariadicFn = savedVariadic;
        }
    }
}

void TypeChecker::visit(ExternDecl* node) {
    // Extern functions are already registered in first pass; verify the signature's types.
    for (const auto& param : node->params) {
        if (param.first == "...") continue;
        validateStructType(normalizeType(param.first), node);
        if (std::string vm = voidTypeError(param.first); !vm.empty())
            errorAt(node, "parameter '" + param.second + "' " + vm);
    }
    validateStructType(normalizeType(node->returnType), node);
}

void TypeChecker::visit(IntrinsicDecl* node) {
    // `intrinsic` is a compiler-provided mechanism, not a user extension point:
    // a name with no codegen lowering must be rejected here, not blow up later.
    if (!isSupportedIntrinsic(node->name)) {
        errorAt(node, "unknown intrinsic '" + node->name +
            "': the compiler provides no lowering for it. `intrinsic` cannot "
            "declare new operations — use `extern` for an external C symbol.");
    }
}

void TypeChecker::foldEnumValues(EnumDecl* ed, bool report) {
    bool any = false;
    for (const auto& v : ed->valueExprs) if (v) any = true;
    if (!any) return;
    bool known = true;
    bool seenExpr = false;
    long long prev = 0;
    for (size_t i = 0; i < ed->members.size(); ++i) {
        auto& m = ed->members[i];
        Expr* ve = i < ed->valueExprs.size() ? ed->valueExprs[i].get() : nullptr;
        long long v = m.second;
        if (ve) {
            seenExpr = true;
            size_t before = errors.size();
            if (report) ve->accept(this);
            long long r = 0;
            known = errors.size() == before && foldConstInt(ve, r);
            if (known) v = r;
            else if (report && errors.size() == before)
                errorAt(ve, "value of enum member '" + m.first + "' of '" + ed->name +
                            "' is not an integer constant expression");
        } else if (seenExpr) {
            v = prev + 1;
        }
        if (known || !seenExpr) m.second = v;
        prev = m.second;
        enumConstants[m.first] = m.second;
    }
}

void TypeChecker::visit(EnumDecl* node) {
    // Members and the enum type were registered in the first pass.
    definitionLocations[node->name] = {node->line, node->col, diagFile()};
    checkTypeParams(node, node->name, node->typeParams);
    if (!node->isADT() && node->typeParams.empty()) foldEnumValues(node, /*report=*/true);
    // A classic enum is an `int`: every member value (explicit, or the previous one + 1)
    // must fit it, instead of wrapping or being truncated.
    if (!node->isADT())
        for (const auto& m : node->members)
            if (m.second < INT32_MIN || m.second > INT32_MAX)
                errorAt(node, "enum member '" + m.first + "' of '" + node->name + "' has value " +
                              std::to_string(m.second) + ", out of range for 'int'");
}

void TypeChecker::visit(TypeAliasDecl* node) {
    // The alias was registered in the first pass; validate the underlying type.
    validateStructType(normalizeType(node->aliased), node);
}

void TypeChecker::visit(InterfaceDecl* node) {
    // Interface registered in first pass; no body to type-check. A method is required once.
    std::set<std::string> names;
    for (const auto& m : node->methods)
        if (!names.insert(m.name).second)
            errorAt(node, "duplicate method '" + m.name + "' in interface '" + node->name + "'");
}

void TypeChecker::visit(UnionDecl* node) {
    // Registered as a struct in the first pass (so field access works and a signature
    // declared before it may name it); here only its field types are validated.
    std::set<std::string> names;
    for (const auto& f : node->fields)
        if (!names.insert(f.name).second)
            errorAt(node, "duplicate field '" + f.name + "' in union '" + node->name + "'");
    checkUnnamedBitfields(node, node->name, node->pads);
    for (const auto& f : node->fields) {
        validateStructType(normalizeType(f.type), node);
        if (std::string vm = voidTypeError(f.type); !vm.empty())
            errorAt(node, "field '" + f.name + "' of '" + node->name + "' " + vm);
    }
}

// The bit width of an integer type a bitfield may have (0: not one).
static int bitfieldTypeWidth(const std::string& normalized) {
    static const std::map<std::string, int> widths = {
        {"bool", 1}, {"char", 8}, {"int8", 8}, {"uint8", 8}, {"int16", 16}, {"uint16", 16},
        {"int", 32}, {"int32", 32}, {"uint", 32}, {"uint32", 32}, {"int64", 64}, {"uint64", 64}};
    auto w = widths.find(normalized);
    return w == widths.end() ? 0 : w->second;
}

void TypeChecker::checkBitfield(ASTNode* at, const std::string& owner, const StructDecl::Field& f) {
    // `at` is null for a generic struct's instance: report where the type is used.
    auto errorAt = [&](ASTNode* node, const std::string& m) {
        if (node) this->errorAt(node, m); else errorAtCtx(m);
    };
    if (f.bitWidth < 0) {
        errorAt(at, "bitfield '" + f.name + "' of '" + owner + "' has zero width (only an unnamed bitfield may be zero-width)");
        return;
    }
    if (f.bitWidth == 0) return;
    int w = bitfieldTypeWidth(normalizeType(f.type));
    if (w == 0) {
        errorAt(at, "bitfield '" + f.name + "' of '" + owner + "' must have an integer type, got '" + f.type + "'");
        return;
    }
    if (f.bitWidth > w)
        errorAt(at, "bitfield '" + f.name + "' of '" + owner + "' is " + std::to_string(f.bitWidth) +
                    " bits wide, more than its type '" + f.type + "' holds (" + std::to_string(w) + ")");
}

void TypeChecker::checkUnnamedBitfields(ASTNode* at, const std::string& owner,
                                        const std::vector<StructDecl::Pad>& pads) {
    for (const auto& p : pads) {
        int w = bitfieldTypeWidth(normalizeType(p.type));
        if (w == 0)
            errorAt(at, "unnamed bitfield of '" + owner + "' must have an integer type, got '" + p.type + "'");
        else if (p.bitWidth > w)
            errorAt(at, "unnamed bitfield of '" + owner + "' is " + std::to_string(p.bitWidth) +
                        " bits wide, more than its type '" + p.type + "' holds (" + std::to_string(w) + ")");
    }
}
