#include "eir_c_int.h"

namespace eir {

void CEmitterImpl::emitCall(const Instruction* inst, std::ostream& os) {
    const auto& ops = inst->operands;
    std::string res = inst->resultVar.empty() ? "" : var(inst->resultVar);
    const std::string& target = ops[0].text;

    if (inst->op == Opcode::Call && isVaCall(target) && !isDefined(target)) {
        std::string ap = "(*(va_list*)" + pval(ops[1]) + ")";
        if (target == "va_start" || target == "llvm.va_start.p0") {
            os << "    va_start(" << ap << ", " << lastParamName() << ");\n";
        } else if (target == "va_end" || target == "llvm.va_end.p0") {
            os << "    va_end(" << ap << ");\n";
        } else {
            TypePtr t = inst->type;
            if (isAggregate(t)) throw std::runtime_error("EIR C: va_arg of an aggregate is not supported");
            std::string promoted = (t && t->isFloat()) ? "double" : (t && t->isInt() && t->bitWidth < 32) ? "int" : cType(t);
            std::string e = "va_arg(" + ap + ", " + promoted + ")";
            if (res.empty()) os << "    (void)" << e << ";\n";
            else os << "    " << res << " = " << (t && t->isInt() ? fromC("(" + cType(t) + ")" + e, t) : "(" + cType(t) + ")" + e) << ";\n";
        }
        return;
    }

    AtomicIntrinsic at;
    if (inst->op == Opcode::Call && parseAtomicIntrinsic(target, at) && !currentMod->getFunction(target)) {
        emitAtomic(inst, at, os);
        return;
    }

    const Function* callee = inst->op == Opcode::Call ? currentMod->getFunction(target) : nullptr;
    bool external = inst->op == Opcode::Call && (!callee || callee->isExtern || callee->blocks.empty());
    bool sret = isAggregate(inst->type);
    bool cRet = external && isCAggregate(inst->type) && !isRuntimeName(target);

    std::vector<std::string> args, argTypes;
    if (sret && !external) {
        args.push_back(res);
        argTypes.push_back("void*");
    }
    bool closure = inst->op == Opcode::CallIndirect && ops[0].type && ops[0].type->isClosure();
    std::string fnExpr;
    if (closure) {
        std::string c = pval(ops[0]);
        fnExpr = "((void**)" + c + ")[0]";
        args.push_back("((void**)" + c + ")[1]");
        argTypes.push_back("void*");
    }
    for (size_t i = 1; i < ops.size(); ++i) {
        bool fixed = callee ? i - 1 < callee->params.size() : inst->op == Opcode::CallIndirect;
        TypePtr pt = (callee && fixed) ? callee->params[i - 1].second : ops[i].type;
        if (external && isVaListType(pt)) {
            args.push_back("(*(va_list*)" + pval(ops[i]) + ")");
            argTypes.push_back("va_list");
            continue;
        }
        if (external && isCAggregate(pt) && !isRuntimeName(target)) {
            std::string rec = cRecord(pt);
            args.push_back("(*(" + rec + "*)" + pval(ops[i]) + ")");
            argTypes.push_back(rec);
            continue;
        }
        if (!fixed && ops[i].kind == OperandKind::ConstantFloat) pt = Type::getF64();
        if (!fixed && opKind(ops[i]) != 'i' && pt && pt->isInt()) pt = Type::getPtr();
        if (!fixed && pt && pt->isInt()) pt = Type::getI64();
        if (isAggregate(pt)) pt = Type::getPtr();
        args.push_back(asC(ops[i], pt));
        argTypes.push_back(cType(pt));
    }

    std::string retC = sret ? "void*" : cType(inst->type);
    std::string call;
    if (inst->op == Opcode::CallIndirect) {
        if (!closure) fnExpr = pval(ops[0]);
        std::string sig = retC + " (*)(";
        for (size_t i = 0; i < argTypes.size(); ++i) sig += (i ? ", " : "") + argTypes[i];
        if (argTypes.empty()) sig += "void";
        sig += ")";
        call = "((" + sig + ")" + fnExpr + ")(";
    } else {
        call = cName(target) + "(";
    }
    for (size_t i = 0; i < args.size(); ++i) call += (i ? ", " : "") + args[i];
    call += ")";

    if (cRet) {
        os << "    *(" << cRecord(inst->type) << "*)" << res << " = " << call << ";\n";
    } else if (!res.empty() && !sret) {
        char k = varKind.count(inst->resultVar) ? varKind[inst->resultVar] : 'p';
        std::string e = fromC(call, inst->type);
        if (k == 'p' && inst->type && inst->type->isInt()) e = "(void*)(intptr_t)" + e;
        os << setInt(inst, e);
    } else if (sret && external) {
        os << "    " << res << " = " << call << ";\n";
    } else {
        os << "    " << call << ";\n";
    }

    bool mayThrow = inst->op == Opcode::CallIndirect || (!external && !isRuntimeName(target));
    if (mayThrow) exceptionCheck(os);
}

void CEmitterImpl::emitAtomic(const Instruction* inst, const AtomicIntrinsic& a, std::ostream& os) {
    const auto& ops = inst->operands;
    std::string res = inst->resultVar.empty() ? "" : var(inst->resultVar);
    if (a.op == "fence") {
        os << "    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n";
        return;
    }
    std::string ct = a.kind == AtomicKind::I32 ? "int32_t" : a.kind == AtomicKind::I64 ? "int64_t" : "void*";
    std::string cell = "((" + ct + "*)" + pval(ops[1]) + ")";
    auto val = [&](size_t i) {
        if (a.kind == AtomicKind::Ptr) return pval(ops[i]);
        return "(" + ct + ")" + ival(ops[i]);
    };
    auto store = [&](const std::string& e) {
        if (res.empty()) os << "    (void)" << e << ";\n";
        else if (a.kind == AtomicKind::Ptr && varKind[inst->resultVar] == 'p') os << setInt(inst, e);
        else if (a.kind == AtomicKind::Ptr) os << "    " << res << " = (int64_t)(intptr_t)" << e << ";\n";
        else os << setInt(inst, "(int64_t)" + e);
    };
    std::string rmw = a.relaxed ? "__ATOMIC_RELAXED" : "__ATOMIC_ACQ_REL";
    if (a.op == "load") {
        store("__atomic_load_n(" + cell + ", " + (a.relaxed ? "__ATOMIC_RELAXED" : "__ATOMIC_ACQUIRE") + ")");
    } else if (a.op == "store") {
        os << "    __atomic_store_n(" << cell << ", " << val(2) << ", " << (a.relaxed ? "__ATOMIC_RELAXED" : "__ATOMIC_RELEASE") << ");\n";
    } else if (a.op == "swap") {
        store("__atomic_exchange_n(" + cell + ", " + val(2) + ", " + rmw + ")");
    } else if (a.op == "cas") {
        std::string tmp = "c" + std::to_string(nextTmp++);
        os << "    { " << ct << " " << tmp << " = " << val(2) << "; int ok_" << tmp << " = __atomic_compare_exchange_n(" << cell
           << ", &" << tmp << ", " << val(3) << ", 0, " << rmw << ", " << (a.relaxed ? "__ATOMIC_RELAXED" : "__ATOMIC_ACQUIRE") << ");";
        if (!res.empty()) os << " " << res << " = ok_" << tmp << ";";
        os << " }\n";
    } else {
        store("__atomic_" + a.op + "(" + cell + ", " + val(2) + ", " + rmw + ")");
    }
}

void CEmitterImpl::emitInlineAsm(const Instruction* inst, std::ostream& os) {
    std::vector<std::string> cons;
    {
        std::string cur;
        int depth = 0;
        for (char c : inst->asmConstraints) {
            if (c == '{') depth++;
            if (c == '}') depth--;
            if (c == ',' && depth == 0) { cons.push_back(cur); cur.clear(); continue; }
            cur += c;
        }
        if (!cur.empty()) cons.push_back(cur);
    }
    std::vector<TypePtr> resultTypes;
    if (inst->type && inst->type->isStruct()) resultTypes = inst->type->members;
    else if (inst->type && !inst->type->isVoid()) resultTypes.push_back(inst->type);

    std::string res = inst->resultVar.empty() ? "" : var(inst->resultVar);
    std::vector<std::string> outs, ins, clobbers;
    std::vector<TypePtr> outTys;
    size_t opIdx = 0, outIdx = 0;
    std::ostringstream pre, post;
    for (const auto& c : cons) {
        if (c.rfind("~{", 0) == 0) {
            clobbers.push_back(escape(c.substr(2, c.size() - 3)));
            continue;
        }
        if (c == "=*m") {
            outs.push_back("\"=m\"(*(char*)" + pval(inst->operands[opIdx++]) + ")");
            outTys.push_back(nullptr);
        } else if (!c.empty() && c[0] == '=') {
            TypePtr t = outIdx < resultTypes.size() ? resultTypes[outIdx] : Type::getI64();
            std::string tmp = "a" + std::to_string(nextTmp++);
            pre << " " << cType(t) << " " << tmp << ";";
            outs.push_back(escape(gccConstraint(c)) + "(" + tmp + ")");
            outTys.push_back(t);
            std::string conv = t && t->isInt() ? "(int64_t)" + tmp : tmp;
            if (resultTypes.size() > 1) post << " ((int64_t*)" << res << ")[" << outIdx << "] = (int64_t)" << tmp << ";";
            else if (!res.empty()) post << " " << res << " = " << conv << ";";
            outIdx++;
        } else if (!c.empty() && isdigit(static_cast<unsigned char>(c[0]))) {
            size_t tied = static_cast<size_t>(std::stoi(c));
            TypePtr t = tied < outTys.size() && outTys[tied] ? outTys[tied] : inst->operands[opIdx].type;
            ins.push_back(escape(c) + "(" + asC(inst->operands[opIdx++], t) + ")");
        } else if (c == "*m" || c == "m") {
            ins.push_back("\"m\"(*(char*)" + pval(inst->operands[opIdx++]) + ")");
        } else {
            const Operand& o = inst->operands[opIdx++];
            ins.push_back(escape(gccConstraint(c)) + "(" + asC(o, o.type) + ")");
        }
    }

    std::string text;
    const std::string& a = inst->asmString;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] == '$' && i + 1 < a.size() && a[i + 1] == '$') { text += '$'; ++i; continue; }
        if (a[i] == '%') { text += "%%"; continue; }
        if (a[i] == '$' && i + 1 < a.size() && isdigit(static_cast<unsigned char>(a[i + 1]))) { text += '%'; continue; }
        if (a[i] == '$' && i + 1 < a.size() && a[i + 1] == '{') {
            size_t e = a.find('}', i);
            std::string inner = a.substr(i + 2, e - i - 2);
            size_t colon = inner.find(':');
            if (colon != std::string::npos) text += "%" + inner.substr(colon + 1) + inner.substr(0, colon);
            else text += "%" + inner;
            i = e;
            continue;
        }
        text += a[i];
    }
    bool extended = !outs.empty() || !ins.empty() || !clobbers.empty();
    os << "    {" << pre.str() << " __asm__ volatile (" << escape(extended ? text : a);
    if (extended) {
        os << " :";
        for (size_t i = 0; i < outs.size(); ++i) os << (i ? ", " : " ") << outs[i];
        os << " :";
        for (size_t i = 0; i < ins.size(); ++i) os << (i ? ", " : " ") << ins[i];
        os << " :";
        for (size_t i = 0; i < clobbers.size(); ++i) os << (i ? ", " : " ") << clobbers[i];
    }
    os << ");" << post.str() << " }\n";
}

} // namespace eir
