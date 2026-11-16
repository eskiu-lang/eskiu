#include "eir_c_int.h"

namespace eir {

void CEmitterImpl::emitPrototype(const Function* fn, std::ostream& os) {
    bool external = fn->isExtern || fn->blocks.empty();
    if (fn->name == "main" && !external) return;
    bool sret = !external && isAggregate(fn->returnType);
    // An extern takes and returns a struct or union by value as C does: spelled as the
    // C record, so the C compiler applies the target ABI.
    std::string ret = sret ? "void*" : (external && isCAggregate(fn->returnType)) ? cRecord(fn->returnType) : cType(fn->returnType);
    os << (external ? "extern " : "") << ret << " " << cName(fn->name) << "(";
    std::vector<std::string> ps;
    if (sret) ps.push_back("void*");
    for (const auto& p : fn->params)
        ps.push_back(external && isVaListType(p.second) ? "va_list" : (external && isCAggregate(p.second)) ? cRecord(p.second) : cType(p.second));
    if (ps.empty() && !fn->isVariadic) os << "void";
    for (size_t i = 0; i < ps.size(); ++i) os << (i ? ", " : "") << ps[i];
    if (fn->isVariadic) {
        if (ps.empty()) throw std::runtime_error("EIR C: variadic function '" + fn->name + "' needs a fixed parameter in C99");
        os << ", ...";
    }
    os << ");\n";
}

void CEmitterImpl::emitImplicitDecls(const Module* mod, std::ostream& os) {
    std::unordered_set<std::string> done;
    for (const auto& fn : mod->functions) {
        for (const auto& bb : fn->blocks) {
            for (const auto& inst : bb->instructions) {
                for (size_t i = 0; i < inst->operands.size(); ++i) {
                    const Operand& op = inst->operands[i];
                    if (op.kind != OperandKind::GlobalRef) continue;
                    const std::string& n = op.text;
                    AtomicIntrinsic at;
                    if (mod->getFunction(n) || isGlobal(n) || isRuntimeName(n) || isVaCall(n) || parseAtomicIntrinsic(n, at)) continue;
                    if (!done.insert(n).second) continue;
                    if (inst->op == Opcode::Call && i == 0) {
                        os << "extern " << (isAggregate(inst->type) ? "void*" : cType(inst->type)) << " " << cName(n) << "();\n";
                    } else {
                        os << "extern char " << cName(n) << "[];\n";
                    }
                }
            }
        }
    }
    if (!done.empty()) os << "\n";
}

std::string CEmitterImpl::memberDecl(const TypePtr& t, const std::string& name) {
    std::string dims;
    TypePtr e = t;
    while (e && e->isArray()) {
        dims += "[" + std::to_string(e->arraySize) + "]";
        e = e->elemType;
    }
    if (e && (e->isClosure() || e->isInterface() || e->isSlice())) return "void* " + name + dims + "[2]";
    if (e && e->isStruct()) return cRecord(e) + " " + name + dims;
    if (e && e->isInt() && e->bitWidth == 1) return "_Bool " + name + dims;
    return cType(e) + " " + name + dims;
}

std::string CEmitterImpl::cRecord(const TypePtr& t) {
    std::string n = t->nominalName();
    auto it = recordNames.find(n);
    if (it != recordNames.end()) return it->second;
    auto lit = currentMod->structLayouts.find(n);
    bool isUnion = lit != currentMod->structLayouts.end() && lit->second.isUnion;
    std::string spelled = std::string(isUnion ? "union" : "struct") + " __eskc_r_" + sanitize("", n);
    recordNames[n] = spelled;
    std::ostringstream body;
    int pack = 0;
    if (lit == currentMod->structLayouts.end()) {
        body << " uint8_t b[" << sizeOf(t) << "];";
    } else {
        const StructInfo& si = lit->second;
        pack = si.packAlign;
        bool adt = si.fields.size() == 1 && si.fields[0].name == "tag" && si.totalSize > 8;
        if (adt) {
            body << " int32_t tag; int64_t payload[" << (si.totalSize - 8) / 8 << "];";
        } else {
            size_t p = 0;
            for (size_t i = 0; i <= si.fields.size(); ++i) {
                for (; p < si.pads.size() && si.pads[p].before <= i; ++p)
                    body << " " << scalarField(si.pads[p].type) << " : " << std::max(si.pads[p].width, 0) << ";";
                if (i == si.fields.size()) break;
                const StructFieldInfo& f = si.fields[i];
                if (f.isBitfield) body << " " << scalarField(f.type) << " " << sanitize("f_", f.name) << " : " << f.bitWidth << ";";
                else body << " " << memberDecl(f.type, sanitize("f_", f.name)) << ";";
            }
        }
    }
    if (pack >= 1) recordDefs << "#pragma pack(push, " << pack << ")\n";
    recordDefs << spelled << " {" << body.str() << " };\n";
    if (pack >= 1) recordDefs << "#pragma pack(pop)\n";
    return spelled;
}

void CEmitterImpl::findCabiFns(const Module* mod) {
    cabiFns.clear();
    cabiOrder.clear();
    for (const auto& fn : mod->functions) {
        for (const auto& bb : fn->blocks) {
            for (const auto& inst : bb->instructions) {
                for (size_t i = 0; i < inst->operands.size(); ++i) {
                    const Operand& op = inst->operands[i];
                    if (op.kind != OperandKind::GlobalRef) continue;
                    if (i == 0 && (inst->op == Opcode::Call || inst->op == Opcode::MakeClosure)) continue;
                    const Function* f = mod->getFunction(op.text);
                    if (!f || f->isExtern || f->blocks.empty()) continue;
                    if (cabiSignature(f) && cabiFns.insert(f->name).second) cabiOrder.push_back(f->name);
                }
            }
        }
    }
}

std::vector<std::string> CEmitterImpl::thunkOrder() const {
    std::vector<std::string> out = cabiOrder;
    std::unordered_set<std::string> seen(cabiOrder.begin(), cabiOrder.end());
    for (const auto& fn : currentMod->functions)
        if (exportEntry(fn.get()) && seen.insert(fn->name).second) out.push_back(fn->name);
    return out;
}

void CEmitterImpl::emitCabiThunk(const Function* fn, std::ostream& os) {
    bool recRet = isCAggregate(fn->returnType);
    std::string ret = recRet ? cRecord(fn->returnType) : cType(fn->returnType);
    if (exportEntry(fn)) os << ret << " " << exportName(fn->name) << "(";
    else os << "static " << ret << " __eskc_cabi_" << cName(fn->name) << "(";
    for (size_t i = 0; i < fn->params.size(); ++i) {
        const TypePtr& pt = fn->params[i].second;
        os << (i ? ", " : "") << (isVaListType(pt) ? "va_list" : isCAggregate(pt) ? cRecord(pt) : cType(pt)) << " a" << i;
    }
    if (fn->params.empty()) os << "void";
    os << ") {\n";
    // A va_list arrives as C passes one; the Eskiu function reads its own copy.
    for (size_t i = 0; i < fn->params.size(); ++i)
        if (isVaListType(fn->params[i].second)) os << "    va_list v" << i << "; va_copy(v" << i << ", a" << i << ");\n";
    std::vector<std::string> args;
    if (recRet) {
        os << "    " << ret << " r;\n";
        args.push_back("(void*)&r");
    }
    for (size_t i = 0; i < fn->params.size(); ++i) {
        const TypePtr& pt = fn->params[i].second;
        std::string n = std::to_string(i);
        args.push_back(isVaListType(pt) ? "(void*)&v" + n : isCAggregate(pt) ? "(void*)&a" + n : "a" + n);
    }
    std::string call = cName(fn->name) + "(";
    for (size_t i = 0; i < args.size(); ++i) call += (i ? ", " : "") + args[i];
    call += ")";
    std::string ends;
    for (size_t i = 0; i < fn->params.size(); ++i)
        if (isVaListType(fn->params[i].second)) ends += "    va_end(v" + std::to_string(i) + ");\n";
    bool isVoid = !fn->returnType || fn->returnType->isVoid();
    if (recRet) os << "    " << call << ";\n" << ends << "    return r;\n";
    else if (isVoid) os << "    " << call << ";\n" << ends;
    else os << "    " << ret << " rv = " << call << ";\n" << ends << "    return rv;\n";
    os << "}\n";
}

void CEmitterImpl::emitGlobalDecls(const Module* mod, std::ostream& os) {
    globalInits.clear();
    for (size_t i = 0; i < mod->globals.size(); ++i) {
        const GlobalVar& g = mod->globals[i];
        size_t sz = sizeOf(g.type);
        std::vector<std::pair<std::string, std::string>> fields;
        size_t used = 0;
        auto addField = [&](const Operand& v, TypePtr ty) {
            if (v.kind == OperandKind::ConstantString || v.kind == OperandKind::GlobalRef || v.kind == OperandKind::ConstantNull)
                ty = Type::getPtr();
            size_t esz = sizeOf(ty);
            if (used + esz > sz) return;
            std::string init;
            if (ty && ty->isFloat()) {
                double d = v.kind == OperandKind::ConstantFloat ? v.floatVal : static_cast<double>(v.intVal);
                init = floatConst(d, ty->bitWidth == 32);
            } else if (ty && ty->isInt()) {
                init = "((" + cType(ty) + ")" + i64Const(truncConst(v.intVal, ty)) + ")";
            } else if (v.kind == OperandKind::ConstantString) {
                init = "((void*)" + escape(v.text) + ")";
            } else if (v.kind == OperandKind::GlobalRef) {
                init = symAddr(v.text);
            } else if (v.kind == OperandKind::ConstantInt) {
                init = "((void*)(uintptr_t)" + std::to_string(v.intVal) + ")";
            } else {
                init = "((void*)0)";
            }
            fields.push_back({cType(ty), init});
            used += esz;
        };
        if (!g.elements.empty()) {
            for (const auto& e : g.elements) addField(e, e.type ? e.type : Type::getPtr());
        } else if (g.init.kind != OperandKind::ConstantNull && !isAggregate(g.type)) {
            addField(g.init, g.type);
        }
        os << "#pragma pack(push, 1)\n";
        os << "typedef union { struct {";
        for (size_t k = 0; k < fields.size(); ++k) os << " " << fields[k].first << " f" << k << ";";
        if (fields.empty()) os << " uint8_t f0;";
        os << " } v; uint8_t b[" << std::max<size_t>(sz, 1) << "]; } " << globalTypeName(i) << ";\n";
        os << "#pragma pack(pop)\n";
        os << "extern " << globalTypeName(i) << " " << cName(g.name) << ";\n";
        globalInits.push_back(fields);
    }
    os << "\n";
}

void CEmitterImpl::emitGlobalDefs(const Module* mod, std::ostream& os) {
    for (size_t i = 0; i < mod->globals.size(); ++i) {
        const auto& fields = globalInits[i];
        os << "#if defined(__GNUC__) || defined(__clang__)\n__attribute__((aligned(16)))\n#endif\n";
        os << globalTypeName(i) << " " << cName(mod->globals[i].name) << " = { { ";
        if (fields.empty()) os << "0";
        for (size_t k = 0; k < fields.size(); ++k) os << (k ? ", " : "") << fields[k].second;
        os << " } };\n";
    }
    os << "\n";
}

} // namespace eir
