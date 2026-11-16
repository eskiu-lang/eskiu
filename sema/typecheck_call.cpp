// typecheck_call.cpp: the call expression visitor, visit(CallExpr) (split from typecheck_expr.cpp).
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

void TypeChecker::visit(CallExpr* node) {
    // The callee may assign any global: a global's narrowing ends after the call.
    struct GlobalNarrowDrop { TypeChecker* t; ~GlobalNarrowDrop() { t->dropGlobalNarrowings(); } } dropAfter{this};
    fillDefaultArgs(node, "");
    // A `?:` argument to an interface parameter of a named function boxes arm by arm.
    if (auto* cid = nodeCast<IdentExpr>(node->callee.get()); cid && lookupSymbol(cid->name).empty()) {
        auto sig = functionSignatures.find(cid->name);
        if (sig != functionSignatures.end())
            for (size_t i = 0; i < node->args.size() && i < sig->second.second.size(); ++i) {
                hintIfaceTarget(node->args[i].get(), sig->second.second[i]);
                if (!funcTemplateDecls.count(cid->name)) inferVariantTarget(node->args[i], sig->second.second[i]);
            }
    }
    // Variadic access builtins: va_start(ap) / va_end(ap) — void.
    if (auto* bid = nodeCast<IdentExpr>(node->callee.get())) {
        if ((bid->name == "va_start" || bid->name == "va_end") && lookupSymbol(bid->name).empty()) {
            for (auto& a : node->args) a->accept(this);
            checkVaListArg(node, bid->name, node->args);
            if (bid->name == "va_start" && !inVariadicFn)
                errorAt(node, "'va_start' used in a function with no variadic parameter ('...')");
            expressionTypes[node] = "void";
            return;
        }
        // `dbg(e)`: prints `[dbg line:col] e = value` to stderr and yields e, unless the
        // program has its own `dbg`.
        if (bid->name == "dbg" && node->args.size() == 1 && lookupSymbol("dbg").empty() &&
            !functionSignatures.count("dbg") && !funcTemplateDecls.count("dbg")) {
            node->args[0]->accept(this);
            std::string t = getExpressionType(node->args[0].get());
            if (normalizeType(t) == "void") errorAt(node->callee.get(), "dbg needs a value: its argument is a void call");
            node->dbgBuiltin = true;
            expressionTypes[node] = t;
            return;
        }
        // `src_file()` / `src_line()` / `src_col()`: the position of a call, so only a
        // parameter's default value (paramDefaultsOf) can use one.
        if (callSiteBuiltin(node) && lookupSymbol(bid->name).empty()) {
            errorAt(bid, "the call-site value '" + bid->name + "()' is only allowed as a parameter's default value, "
                          "where it gives the position of each call");
            expressionTypes[node] = bid->name == "src_file" ? "string" : "int";
            return;
        }
        // `exception_text()`: inside a `catch` block, yields a string describing the caught
        // exception (as the uncaught reporter prints it).
        if (bid->name == "exception_text" && lookupSymbol("exception_text").empty() &&
            !functionSignatures.count("exception_text") && !funcTemplateDecls.count("exception_text")) {
            for (auto& a : node->args) a->accept(this);
            if (!node->args.empty())
                errorAt(node, "'exception_text' takes no arguments, got " + std::to_string(node->args.size()));
            if (catchDepth == 0)
                errorAt(node, "'exception_text' is only allowed inside a 'catch' block");
            expressionTypes[node] = "string";
            return;
        }
        // `__exception_text_into(buf)`: the async lowering's copy of the text into a frame field
        // of 512 chars, in the catch handler it synthesizes.
        if (bid->name == "__exception_text_into" && node->args.size() == 1 && catchDepth > 0) {
            node->args[0]->accept(this);
            expressionTypes[node] = "void";
            return;
        }
    }
    // Algebraic variant construction: `Circle(2.0)`, `Some(x)`. The callee names a
    // variant; the call yields the enum value, checking the payload field types.
    if (auto cid = nodeCast<IdentExpr>(node->callee.get())) {
        auto vit = adtVariants.find(cid->name);
        if (vit != adtVariants.end() && lookupSymbol(cid->name).empty()) {
            const std::string& enumName = vit->second.first;
            const auto& payload = enumDecls[enumName]->payloads[vit->second.second];
            for (auto& a : node->args) a->accept(this);
            if (payload.empty())                  // a nullary variant is written bare: `B`
                errorAt(node, "variant '" + cid->name + "' takes no arguments");
            else if (node->args.size() != payload.size())
                errorAt(node, "variant '" + cid->name + "' expects " +
                    std::to_string(payload.size()) + " argument(s), got " +
                    std::to_string(node->args.size()));
            else
                for (size_t i = 0; i < payload.size(); ++i) {
                    std::string at = getExpressionType(node->args[i].get());
                    if (at != "unknown" && !isValidAssignment(payload[i], at))
                        errorAt(node, "variant '" + cid->name + "' argument " +
                            std::to_string(i + 1) + " type mismatch");
                    else checkVariantLiteral(node, cid->name, i, payload[i]);
                }
            expressionTypes[node] = enumName;
            return;
        }
        // Bare generic-variant construction with inference: `Some(5)`. Infer the
        // enum's type args by unifying the variant's payload against the arg types;
        // if they don't fully determine the type, require explicit args (turbofish).
        auto gvit = genericVariants.find(cid->name);
        if (gvit != genericVariants.end() && lookupSymbol(cid->name).empty()) {
            EnumDecl* ge = genericEnumDecls[gvit->second.first];
            const auto& payload = ge->payloads[gvit->second.second];
            for (auto& a : node->args) a->accept(this);
            std::set<std::string> tps(ge->typeParams.begin(), ge->typeParams.end());
            std::map<std::string, std::string> subs;
            for (size_t i = 0; i < payload.size() && i < node->args.size(); ++i) {
                std::string at = getExpressionType(node->args[i].get());
                if (at != "unknown" && !at.empty()) unifyTypeParam(payload[i], at, tps, subs);
            }
            bool allBound = true;
            for (const auto& tp : ge->typeParams) if (!subs.count(tp)) allBound = false;
            if (!allBound) {
                errorAt(node, "cannot infer type arguments for variant '" + cid->name +
                    "'; write them explicitly, e.g. " + cid->name + "<...>(...)");
                expressionTypes[node] = "unknown";
                return;
            }
            if (node->args.size() != payload.size())
                errorAt(node, "variant '" + cid->name + "' expects " +
                    std::to_string(payload.size()) + " argument(s), got " + std::to_string(node->args.size()));
            std::string inst = gvit->second.first + "<";
            for (size_t i = 0; i < ge->typeParams.size(); ++i) { if (i) inst += ","; inst += subs[ge->typeParams[i]]; }
            inst += ">";
            expressionTypes[node] = normalizeType(inst);
            return;
        }
    }
    // Method call: callee is MemberExpr (e.g. p.distance(q))
    if (auto member = nodeCast<MemberExpr>(node->callee.get())) {
        member->base->accept(this);
        if (member->member == kCompleteSentinel) {         // --complete-at on `recv.` of a call
            if (!completionDone) { completeMembers(getExpressionType(member->base.get())); completionDone = true; }
            for (auto& a : node->args) a->accept(this);
            expressionTypes[node] = "unknown";
            return;
        }
        // A method call dereferences its receiver: a `?*T` must be null-checked first.
        checkNullableDeref(member->base.get(), "access a member of");
        // Dot syntax dereferences at most one pointer level.
        if (std::string rt = getExpressionType(member->base.get());
            rt != "unknown" && !ty::Type::parse(normalizeType(rt)).isFn() && pointerDepth(rt) > 1) {
            errorAt(node, "cannot call method '" + member->member + "' through '" + rt +
                          "': dot syntax dereferences one pointer level (dereference it first)");
            for (auto& a : node->args) a->accept(this);
            expressionTypes[node] = "unknown";
            return;
        }
        // bare nominal: *Rect and Rect both resolve to Rect_method
        std::string baseType = ty::Type::parse(getExpressionType(member->base.get())).nominalName();
        if (typeAliases.count(baseType))   // `*Alias` receiver: the aliased struct's methods
            baseType = ty::Type::parse(normalizeType(baseType)).nominalName();
        {
            // A source-form instance receiver (`Box<int>`, e.g. a field's declared
            // type) names its mangled struct, instantiated so its inline methods exist.
            ty::Type rt = ty::Type::parse(tyq::strip(getExpressionType(member->base.get())));
            while (rt.isPointer() && rt.pointee) { ty::Type inner = *rt.pointee; rt = inner; }
            if (rt.isTemplate()) baseType = ty::Type::parse(normalizeType(rt.str())).nominalName();
        }

        std::string mangled = baseType + "_" + member->member;
        if (auto st = structs.find(baseType); st != structs.end() && !st->second.field(member->member))
            fillDefaultArgs(node, mangled);
        auto mit = functionSignatures.find(mangled);
        if (mit != functionSignatures.end()) {
            const auto& sig = mit->second;
            const auto& paramTypes = sig.second; // first param is "self"
            calledFns.insert(mangled);           // -Wall: `x.m()` references `Type_m`
            noteDeprecatedUse(mangled, member->memberLine > 0 ? member->memberLine : node->line,
                              member->memberLine > 0 ? member->memberCol : node->col);
            recordMemberTooling(member, signatureText(mangled), mangled);
            // Dot syntax passes the receiver's address, so the function's first parameter
            // must be a pointer to the receiver's type (`*P self` or `const P* self`).
            if (paramTypes.empty() || !tyq::isPtr(paramTypes[0]) ||
                ty::Type::parse(normalizeType(tyq::strip(tyq::pointee(paramTypes[0])))).nominalName() != baseType) {
                errorAt(node, "'" + mangled + "' cannot be called as method '" + member->member + "' of '" +
                              baseType + "': its first parameter must be '*" + baseType + " self' (it is '" +
                              (paramTypes.empty() ? std::string("none") : paramTypes[0]) + "')");
                for (auto& a : node->args) a->accept(this);
                expressionTypes[node] = sig.first;
                return;
            }
            if (auto gm = genericMethodInsts.find(mangled); gm != genericMethodInsts.end())
                queueInstance(gm->second.fn, gm->second.owner->typeParams, gm->second.subs,
                              gm->second.owner->name, "." + member->member, mangled,
                              "*" + baseType, gm->second.owner->sourceFile);
            // A method whose `self` is a plain (mutable) pointer may write through it, so
            // it cannot be called on a read-only receiver (a const value, or through a
            // pointer to const) unless it declares `const T* self`.
            {
                std::string rt = getExpressionType(member->base.get());
                std::string cname;
                bool roRecv = tyq::isPtr(rt) ? tyq::baseConst(tyq::pointee(rt))
                                             : assignsToConst(member->base.get(), cname);
                if (roRecv && !paramTypes.empty() && !tyq::baseConst(tyq::pointee(paramTypes[0])))
                    errorAt(node, "cannot call method '" + member->member + "' on a read-only value: its '" +
                                  paramTypes[0] + " self' may modify it (declare it 'const " + baseType +
                                  "* self' if it does not)");
                else if (!tyq::isPtr(rt) && !paramTypes.empty() && !tyq::baseConst(tyq::pointee(paramTypes[0])))
                    checkCapturedMethodCall(node, member);
            }
            size_t selfSkip = 1;
            bool isVariadic = paramTypes.size() > selfSkip && paramTypes.back() == "...";
            size_t fixedCount = isVariadic ? paramTypes.size() - selfSkip - 1
                                           : paramTypes.size() - selfSkip;

            if (!isVariadic && node->args.size() != fixedCount) {
                errorAt(node,"method '" + member->member + "' expects " +
                            std::to_string(fixedCount) + " argument(s), got " +
                            std::to_string(node->args.size()));
            }
            for (size_t i = 0; i < node->args.size(); ++i) {
                node->args[i]->accept(this);
                if (i < fixedCount) {
                    std::string argType = getExpressionType(node->args[i].get());
                    size_t pi = i + selfSkip;
                    if (argType != "unknown" && !isValidAssignment(paramTypes[pi], argType)) {
                        errorAt(node,"argument " + std::to_string(i + 1) + " type mismatch");
                    }
                } else if (isVariadic) {
                    checkVariadicArg(node, node->args[i].get(), i);
                }
            }
            checkFormatCall(node, mangled, node->args, 1);
            expressionTypes[node] = sig.first;
            return;
        }
        if (checkGenericMethodCall(node, member, baseType)) return;
        // Check if baseType is an interface — resolve via interface method list
        {
            auto ifaceIt = interfaceDecls.find(baseType);
            if (ifaceIt != interfaceDecls.end()) {
                for (const auto& sig : ifaceIt->second->methods) {
                    if (sig.name == member->member) {
                        std::string d = sig.returnType + " " + sig.name + "(";
                        for (size_t i = 0; i < sig.params.size(); ++i)
                            d += (i ? ", " : "") + sig.params[i].first +
                                 (sig.params[i].second.empty() ? "" : " " + sig.params[i].second);
                        recordMemberTooling(member, d + ")", "", nullptr);
                        for (auto& arg : node->args) arg->accept(this);
                        std::vector<std::string> pts;
                        for (const auto& p : sig.params) pts.push_back(p.first);
                        checkCallArgs(node, "method '" + member->member + "'", pts);
                        expressionTypes[node] = normalizeType(sig.returnType);
                        return;
                    }
                }
                errorAt(node, "interface '" + baseType + "' has no method '" + member->member + "'");
                expressionTypes[node] = "unknown";
                return;
            }
        }
        // Inside a generic instance, a constrained call `t.m(x)` on a primitive receiver is
        // satisfied by a free function `m(T, ...)` (codegen lowers it to `m(t, x)`).
        static const std::set<std::string> kScalarPrims = {
            "int","int8","int16","int32","int64","uint","uint8","uint16","uint32",
            "uint64","char","bool","float","double"};
        if (inInstance && kScalarPrims.count(baseType)) {
            auto fit = functionSignatures.find(member->member);
            if (fit != functionSignatures.end() && !fit->second.second.empty()) {
                calledFns.insert(member->member);
                const auto& pts = fit->second.second;
                std::string recvT = getExpressionType(member->base.get());
                std::string e = assignabilityError(pts[0], recvT, member->base.get());
                if (!e.empty())
                    errorAt(node, "receiver of '" + member->member + "' type mismatch: expected " +
                                  pts[0] + ", got " + recvT + " (" + e + ")");
                for (auto& arg : node->args) arg->accept(this);
                checkCallArgs(node, "function '" + member->member + "'",
                              std::vector<std::string>(pts.begin() + 1, pts.end()));
                expressionTypes[node] = fit->second.first;
                return;
            }
        }
        // Not a method — maybe a struct field holding a fn pointer: o.op(args).
        size_t errsBeforeMember = errors.size();
        member->accept(this);
        std::string fieldTy = dealiasOperand(getExpressionType(member));
        if (fieldTy.size() > 3 && fieldTy.substr(0, 3) == "fn(") {
            for (auto& arg : node->args) arg->accept(this);
            expressionTypes[node] = checkFnValueCall(node, "'" + member->member + "'", fieldTy);
            return;
        }
        // On a non-struct receiver the error is the undefined method (as the self-host
        // reports it), not also a member access.
        // The same holds on a struct with no such field: the error is the method.
        std::string memberErr = "cannot access member '" + member->member + "' on non-struct type";
        std::string fieldErr = "has no member '" + member->member + "'";
        for (size_t k = errsBeforeMember; k < errors.size();)
            if (errors[k].find(memberErr) != std::string::npos || errors[k].find(fieldErr) != std::string::npos)
                errors.erase(errors.begin() + (long)k);
            else ++k;
        errorAt(node,"undefined method '" + member->member + "' on type '" + baseType + "'" +
                     didYouMean(member->member, 'm', getExpressionType(member->base.get())));
        expressionTypes[node] = "unknown";
        return;
    }

    // Regular function call
    std::string funcName;
    if (auto identExpr = nodeCast<IdentExpr>(node->callee.get())) {
        funcName = identExpr->name;
        if (funcName == kCompleteSentinel) {               // --complete-at on a callee
            identExpr->accept(this);
            for (auto& a : node->args) a->accept(this);
            expressionTypes[node] = "unknown";
            return;
        }
        calledFns.insert(funcName);  // -Wall: mark referenced
        noteDeprecatedUse(funcName, identExpr->line, identExpr->col);
        // Record use-site for go-to-definition
        if (inPrimaryFile()) useLocations[{identExpr->line, identExpr->col}] = funcName;
    } else {
        // Any other callee expression (`mk()(1)`, `(*pf)(x)`): call its fn-typed value.
        node->callee->accept(this);
        for (auto& a : node->args) a->accept(this);
        std::string ct = getExpressionType(node->callee.get());
        expressionTypes[node] = ty::Type::parse(normalizeType(ct)).isFn()
            ? checkFnValueCall(node, "the called function value", ct) : "unknown";
        return;
    }

    // Check if funcName is a variable holding a function pointer (fn(T,...)->R)
    {
        std::string varType = lookupSymbol(funcName);
        if (!varType.empty() && varType.size() > 3 && varType.substr(0, 3) == "fn(") {
            // The callee is a fn-pointer *variable*. Visit it so that, inside a
            // lambda, an outer-scope fn pointer used in callee position is
            // registered as a capture (visit(CallExpr) otherwise resolves the
            // name directly and never reaches visit(IdentExpr)).
            // calleeContext marks this occurrence as a *call* of the var, so a
            // watched closure param used here is not counted as escaping.
            { std::string prev = calleeContext; calleeContext = funcName;
              node->callee->accept(this); calleeContext = prev; }
            for (auto& a : node->args) a->accept(this);
            expressionTypes[node] = checkFnValueCall(node, "'" + funcName + "'", varType);
            return;
        }
        // A variable that is not a closure (a local may shadow a function of the same
        // name) cannot be called.
        if (!varType.empty() && !ty::Type::parse(normalizeType(varType)).isFn()) {
            for (auto& a : node->args) a->accept(this);
            errorAt(node->callee.get(), "undefined function '" + funcName + "' ('" + funcName +
                                        "' is a variable of type '" + varType + "')");
            expressionTypes[node] = "unknown";
            return;
        }
    }

    // Look up function signature
    auto it = functionSignatures.find(funcName);
    if (it == functionSignatures.end()) {
        // Template function called without explicit type arguments: infer each type
        // parameter from an argument whose parameter type is exactly that type param.
        auto tmplIt = funcTemplateDecls.find(funcName);
        if (tmplIt != funcTemplateDecls.end()) {
            FunctionDecl* fd = tmplIt->second;
            for (size_t j = 0; j < node->args.size(); ++j) visitGenericCallArg(fd, j, node->args[j]);
            std::set<std::string> tps(fd->typeParams.begin(), fd->typeParams.end());
            std::map<std::string, std::string> subs;
            // A parameter spelled as a bare type parameter (`T a`) is a by-value deduction;
            // the others bind structurally first and take precedence.
            auto bareParam = [&](size_t j) {
                std::string p = tyq::strip(fd->params[j].first);
                return tps.count(p) ? p : std::string();
            };
            for (size_t j = 0; j < fd->params.size() && j < node->args.size(); ++j) {
                std::string at = getExpressionType(node->args[j].get());
                if (at != "unknown" && !at.empty() && bareParam(j).empty())
                    unifyTypeParam(fd->params[j].first, at, tps, subs);
            }
            // By-value deductions of one type parameter must agree; integer ones that differ
            // meet at their common type by C's usual arithmetic conversions
            // (`maxof(1, big)` is `maxof<int64>`), any other disagreement is an error.
            std::map<std::string, std::pair<std::string, size_t>> byValue;
            bool conflict = false;
            for (size_t j = 0; j < fd->params.size() && j < node->args.size(); ++j) {
                std::string tp = bareParam(j);
                if (tp.empty() || subs.count(tp)) continue;
                std::string at = getExpressionType(node->args[j].get());
                if (at == "unknown" || at.empty() || at == "null") continue;
                std::map<std::string, std::string> one;
                unifyTypeParam(fd->params[j].first, at, tps, one);
                if (!one.count(tp)) continue;
                at = one[tp];
                auto bv = byValue.find(tp);
                if (bv == byValue.end()) { byValue[tp] = {at, j}; continue; }
                std::string cur = bv->second.first;
                if (normalizeType(cur) == normalizeType(at)) continue;
                std::string common = ty::rangeVarType(normalizeType(cur), normalizeType(at));
                if (!common.empty()) { bv->second.first = common; continue; }
                errorAt(node, "argument " + std::to_string(j + 1) + " type mismatch: type parameter '" + tp +
                              "' is deduced as '" + cur + "' from argument " +
                              std::to_string(bv->second.second + 1) + " and as '" + at + "' here");
                conflict = true;
            }
            for (const auto& kv : byValue) subs[kv.first] = kv.second.first;
            if (conflict) {
                expressionTypes[node] = "unknown";
                return;
            }
            std::string unbound;
            for (const auto& tpName : fd->typeParams)
                if (!subs.count(tpName)) unbound += (unbound.empty() ? "" : ", ") + tpName;
            if (unbound.empty()) {
                size_t errsBefore = errors.size();
                checkConstraints(node, fd->constraints, subs);
                // The inferred instantiation's parameter types must accept every argument.
                std::vector<std::string> pts;
                for (const auto& p : fd->params) pts.push_back(substType(p.first, subs));
                checkCallArgs(node, "function '" + funcName + "'", pts);
                if (errors.size() == errsBefore) {
                    std::string mangled = funcName;
                    for (const auto& tpn : fd->typeParams) mangled += "_" + mangleTemplate(subs[tpn]);
                    queueInstance(fd, fd->typeParams, subs, funcName, "", mangled, "", fd->sourceFile);
                }
                expressionTypes[node] = genericCallRet(fd, subs);
                return;
            }
            errorAt(node, "cannot infer type argument(s) " + unbound + " of generic function '" +
                          funcName + "' from the call; write them explicitly, e.g. " + funcName + "<...>(...)");
            expressionTypes[node] = "unknown";
            return;
        }
        errorAt(node->callee.get(), "undefined function '" + funcName + "'" + didYouMean(funcName, 'v'));
        expressionTypes[node] = "unknown";
        return;
    }

    const auto& sig = it->second;
    const auto& expectedParamTypes = sig.second;

    bool isVariadic = !expectedParamTypes.empty() && expectedParamTypes.back() == "...";
    size_t fixedCount = isVariadic ? expectedParamTypes.size() - 1 : expectedParamTypes.size();

    // Check argument count
    if (isVariadic) {
        if (node->args.size() < fixedCount) {
            errorAt(node,"function '" + funcName + "' expects at least " +
                        std::to_string(fixedCount) + " arguments, got " +
                        std::to_string(node->args.size()));
            expressionTypes[node] = sig.first;
            return;
        }
    } else if (node->args.size() != fixedCount) {
        errorAt(node,"function '" + funcName + "' expects " +
                    std::to_string(fixedCount) + " arguments, got " +
                    std::to_string(node->args.size()));
        expressionTypes[node] = sig.first;
        return;
    }

    // Per-param escaping flags for this callee (empty if none declared).
    auto escIt = functionParamEscaping.find(funcName);
    const std::vector<bool>* escVec =
        (escIt != functionParamEscaping.end() && !escIt->second.empty())
            ? &escIt->second : nullptr;

    // Type check fixed arguments; visit (but do not type-check) variadic extras
    for (size_t i = 0; i < node->args.size(); ++i) {
        // A watched (non-escaping) closure param passed straight to a non-escaping
        // param of an Eskiu function does not escape either: the callee may only
        // call it. (A C function's fn-typed param is a C function pointer, not this.)
        bool forwards = false;
        if (auto* aid = nodeCast<IdentExpr>(node->args[i].get())) {
            bool paramEscapes = escVec && i < escVec->size() && (*escVec)[i];
            forwards = nonEscapingFnParams.count(aid->name) && !paramEscapes &&
                       !externFnNames.count(funcName) && i < fixedCount;
        }
        if (forwards) {
            std::string prev = calleeContext;
            calleeContext = static_cast<IdentExpr*>(node->args[i].get())->name;
            node->args[i]->accept(this);
            calleeContext = prev;
        } else {
            node->args[i]->accept(this);
        }
        // Escape optimization: a lambda passed directly to a NON-escaping
        // parameter does not outlive the call (the callee may only call it —
        // enforced by the soundness check), so its env can stay on the stack.
        if (auto* lam = nodeCast<LambdaExpr>(node->args[i].get())) {
            bool paramEscapes = escVec && i < escVec->size() && (*escVec)[i];
            // Set both ways: the check after the async lowering sees an async fn's closure
            // params as escaping (the frame keeps them), overriding the first check.
            if (!inInstance) lam->escapes = paramEscapes;
            else {
                // Decided after the instances; an async callee's frame keeps its closures.
                bool asyncCallee = false;
                auto fa = fnDeclsAll.find(funcName);
                if (fa != fnDeclsAll.end())
                    for (const FunctionDecl* d : fa->second) asyncCallee = asyncCallee || d->isAsync;
                deferredLambdas[lam].escapes |= paramEscapes || asyncCallee;
            }
        }
        if (i < fixedCount) {
            std::string argType = getExpressionType(node->args[i].get());
            // A C function's fn-typed parameter is a C function pointer: C can only call
            // a named top-level function (or get null), never a closure's environment.
            if (externFnNames.count(funcName)
                    && ty::Type::parse(normalizeType(expectedParamTypes[i])).isFn()) {
                auto* id = nodeCast<IdentExpr>(node->args[i].get());
                auto* lit = nodeCast<LiteralExpr>(node->args[i].get());
                if (lit && lit->kind == LiteralExpr::Kind::NULL_VAL) continue;
                if (!(id && lookupSymbol(id->name).empty() && functionSignatures.count(id->name))) {
                    errorAt(node, "argument " + std::to_string(i + 1) + " of extern '" + funcName +
                                  "' is a C function pointer: pass a top-level function by name, not a closure value");
                    continue;
                }
            }
            std::string e = assignabilityError(expectedParamTypes[i], argType, node->args[i].get());
            if (!e.empty())
                errorAt(node,"argument " + std::to_string(i + 1) + " type mismatch: expected " +
                            expectedParamTypes[i] + ", got " + argType + " (" + e + ")");
        } else if (isVariadic) {
            checkVariadicArg(node, node->args[i].get(), i);
        }
    }

    if (isVariadic) checkFormatCall(node, funcName, node->args, 0);
    expressionTypes[node] = sig.first;
}
