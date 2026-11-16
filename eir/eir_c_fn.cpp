#include "eir_c_int.h"

namespace eir {

void CEmitterImpl::emitFunction(const Function* fn, std::ostream& os) {
    curFn = fn;
    fnHasSret = isAggregate(fn->returnType);
    fnSavesCaught = false;
    handlerIds.clear();
    varKind.clear();
    nextTmp = 0;
    size_t pushCount = 0;

    std::ostringstream decls;
    for (const auto& p : fn->params) varKind["%" + p.first] = kindOf(p.second);

    for (const auto& bb : fn->blocks) {
        for (const auto& inst : bb->instructions) {
            if (inst->op == Opcode::PushHandler) {
                pushCount++;
                if (!handlerIds.count(inst->operands[0].text)) {
                    int id = static_cast<int>(handlerIds.size());
                    handlerIds[inst->operands[0].text] = id;
                }
            }
            if (inst->op == Opcode::Call && inst->operands[0].text == "__eir_push_caught") fnSavesCaught = true;
            const std::string& rv = inst->resultVar;
            if (rv.empty() || varKind.count(rv)) continue;
            std::string v = var(rv);
            std::string tag = v.substr(2);
            if (inst->op == Opcode::Alloca) {
                decls << "    union { uint8_t b[" << sizeOf(inst->type) << "]; uint64_t a; double d; void* p; } s_" << tag << ";\n";
                decls << "    void* " << v << " = (void*)&s_" << tag << ";\n";
                varKind[rv] = 'p';
            } else if (inst->op == Opcode::MakeClosure || inst->op == Opcode::MakeInterface || inst->op == Opcode::MakeSlice) {
                decls << "    void* f_" << tag << "[2];\n";
                decls << "    void* " << v << " = (void*)f_" << tag << ";\n";
                varKind[rv] = 'p';
            } else if ((inst->op == Opcode::Call || inst->op == Opcode::CallIndirect) && isAggregate(inst->type)) {
                decls << "    union { uint8_t b[" << sizeOf(inst->type) << "]; uint64_t a; void* p; } r_" << tag << ";\n";
                decls << "    void* " << v << " = (void*)&r_" << tag << ";\n";
                varKind[rv] = 'p';
            } else if (inst->op == Opcode::InlineAsm && inst->type && inst->type->isStruct()) {
                decls << "    int64_t o_" << tag << "[" << std::max<size_t>(inst->type->members.size(), 1) << "];\n";
                decls << "    void* " << v << " = (void*)o_" << tag << ";\n";
                varKind[rv] = 'p';
            } else {
                char k = kindOf(inst->type);
                decls << "    " << kindType(k) << " " << v << " = 0;\n";
                varKind[rv] = k;
            }
        }
    }
    if (pushCount > 0) {
        handlerCap = std::max<size_t>(16, pushCount * 4);
        decls << "    int32_t __eskc_hs[" << handlerCap << "];\n    size_t __eskc_hd = 0;\n";
    } else {
        handlerCap = 0;
    }
    if (fnSavesCaught) decls << "    int32_t __eskc_cd = __eir_caught_depth();\n";

    bool isMain = fn->name == "main";
    std::string retType = isMain ? "int" : fnHasSret ? "void*" : cType(fn->returnType);
    os << retType << " " << cName(fn->name) << "(";
    std::vector<std::string> ps;
    if (fnHasSret) ps.push_back("void* __eskc_sret");
    for (size_t i = 0; i < fn->params.size(); ++i) {
        if (isMain && i == 0) ps.push_back("int __eskc_argc");
        else if (isMain && i == 1) ps.push_back("char** __eskc_argv");
        else ps.push_back(cType(fn->params[i].second) + " p_" + var("%" + fn->params[i].first).substr(2));
    }
    if (ps.empty() && !fn->isVariadic) os << "void";
    for (size_t i = 0; i < ps.size(); ++i) os << (i ? ", " : "") << ps[i];
    if (fn->isVariadic) os << ", ...";
    os << ") {\n";

    for (size_t i = 0; i < fn->params.size(); ++i) {
        std::string v = var("%" + fn->params[i].first);
        char k = kindOf(fn->params[i].second);
        std::string src = (isMain && i == 0) ? "__eskc_argc" : (isMain && i == 1) ? "__eskc_argv" : "p_" + v.substr(2);
        if (k == 'i') os << "    int64_t " << v << " = (int64_t)" << src << ";\n";
        else os << "    " << kindType(k) << " " << v << " = (" << kindType(k) << ")" << src << ";\n";
    }
    os << decls.str() << "\n";

    for (const auto& bb : fn->blocks) {
        os << label(bb->name) << ": ;\n";
        for (const auto& inst : bb->instructions) emitInstruction(inst.get(), os);
        if (!bb->hasTerminator()) emitReturn(nullptr, os);
    }

    if (throws || pushCount > 0) {
        os << "__eskc_unwind: ;\n";
        if (pushCount > 0) {
            os << "    if (__eskc_hd > 0) {\n        switch (__eskc_hs[--__eskc_hd]) {\n";
            // In id order, so the output does not depend on the hash map's.
            std::vector<std::string> byId(handlerIds.size());
            for (const auto& kv : handlerIds) byId[kv.second] = kv.first;
            for (size_t id = 0; id < byId.size(); ++id)
                os << "        case " << id << ": goto " << label(byId[id]) << ";\n";
            os << "        }\n        __eskc_trap();\n    }\n";
        }
        if (isMain) os << "    __eir_uncaught();\n";
        emitReturn(nullptr, os);
    }
    os << "}\n\n";
}

void CEmitterImpl::emitReturn(const Operand* val, std::ostream& os) {
    if (fnSavesCaught) os << "    __eir_set_caught_depth(__eskc_cd);\n";
    bool isMain = curFn->name == "main";
    if (fnHasSret) {
        if (val) os << "    __eskc_copy(__eskc_sret, " << pval(*val) << ", " << sizeOf(curFn->returnType) << ");\n";
        os << "    return __eskc_sret;\n";
        return;
    }
    if (!curFn->returnType || curFn->returnType->isVoid()) {
        os << (isMain ? "    return 0;\n" : "    return;\n");
        return;
    }
    if (isMain) {
        os << "    return (int)" << (val ? ival(*val) : std::string("0")) << ";\n";
        return;
    }
    os << "    return " << (val ? asC(*val, curFn->returnType) : "(" + cType(curFn->returnType) + ")0") << ";\n";
}

std::string CEmitterImpl::loadExpr(const TypePtr& t, const std::string& addr) {
    if (t && t->isInt() && t->bitWidth == 1) return "(int64_t)(*(uint8_t*)" + addr + " != 0)";
    if (t && t->isInt()) return "(int64_t)*(" + cType(t) + "*)" + addr;
    return "*(" + cType(t) + "*)" + addr;
}

std::string CEmitterImpl::narrow(const std::string& e, const TypePtr& t) {
    if (!t || !t->isInt() || t->bitWidth >= 64) return e;
    return "(int64_t)(" + cType(t) + ")(" + e + ")";
}

std::string CEmitterImpl::setInt(const Instruction* inst, const std::string& e) {
    char k = varKind.count(inst->resultVar) ? varKind[inst->resultVar] : 'i';
    std::string res = var(inst->resultVar);
    if (k == 'p') return "    " + res + " = (void*)(intptr_t)(" + e + ");\n";
    if (k == 'f' || k == 'd') return "    " + res + " = (" + kindType(k) + ")(" + e + ");\n";
    return "    " + res + " = " + e + ";\n";
}

void CEmitterImpl::emitInstruction(const Instruction* inst, std::ostream& os) {
    emitInstructionBody(inst, os);
    // Every integer value is held normalized to its type: wrapped to its width,
    // extended by its signedness.
    const std::string& rv = inst->resultVar;
    TypePtr t = inst->type;
    if (!rv.empty() && t && t->isInt() && t->bitWidth < 64 && varKind.count(rv) && varKind[rv] == 'i' && !inst->isTerminator()) {
        std::string v = var(rv);
        if (t->bitWidth == 1) os << "    " << v << " &= 1;\n";
        else os << "    " << v << " = (int64_t)(" << cType(t) << ")" << v << ";\n";
    }
}

std::string CEmitterImpl::lastParamName() const {
    if (curFn->params.empty()) throw std::runtime_error("EIR C: va_start in '" + curFn->name + "', which has no fixed parameter");
    if (curFn->name == "main") return "__eskc_argv";
    return "p_" + var("%" + curFn->params.back().first).substr(2);
}

std::string CEmitterImpl::fromC(const std::string& e, const TypePtr& t) {
    if (t && t->isInt()) return "(int64_t)(" + cType(t) + ")" + e;
    return e;
}

} // namespace eir
