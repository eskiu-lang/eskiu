#include "eir_asm_int.h"

namespace eir {

void Arm64Emitter::emitModule(const Module* mod, std::ostream& os) {
    currentMod = mod;
    stringTable.clear();
    stringOrder.clear();
    definedSyms.clear();
    nextStrId = 0;
    nextLocal = 0;
    throws = moduleThrows(mod);

    for (const auto& fn : mod->functions) {
        if (!fn->isExtern && !fn->blocks.empty()) definedSyms.insert(fn->name);
    }
    for (const auto& g : mod->globals) definedSyms.insert(g.name);

    collectStrings(mod);

    if (!stringTable.empty()) {
        os << (isDarwin ? "    .section __TEXT,__cstring,cstring_literals\n"
                        : "    .section .rodata.str1.1,\"aMS\",@progbits,1\n");
        for (const auto& kv : stringOrder) {
            os << kv.second << ":\n    .asciz \"" << escapeAsm(kv.first) << "\"\n";
        }
        os << "\n";
    }

    if (!mod->globals.empty()) {
        os << (isDarwin ? "    .section __DATA,__data\n" : "    .data\n");
        for (const auto& g : mod->globals) emitGlobal(g, os);
        os << "\n";
    }

    os << (isDarwin ? "    .section __TEXT,__text,regular,pure_instructions\n" : "    .text\n");
    findCabiFns(mod);
    int idx = 0;
    std::unordered_set<std::string> emitted;
    for (const auto& fn : mod->functions) {
        if (fn->isExtern || fn->blocks.empty()) continue;
        if (!emitted.insert(fn->name).second) continue;
        emitFunction(fn.get(), idx++, os);
    }
    for (const auto& name : cabiOrder) emitCabiThunk(mod->getFunction(name), os);
    for (const auto& fn : mod->functions)
        if (exportEntry(fn.get()) && !cabiFns.count(fn->name) && emitted.insert("export:" + fn->name).second)
            emitCabiThunk(fn.get(), os);
}

std::string Arm64Emitter::escapeAsm(const std::string& s) {
    std::ostringstream ss;
    for (unsigned char c : s) {
        if (c == '\n') ss << "\\n";
        else if (c == '\t') ss << "\\t";
        else if (c == '\r') ss << "\\r";
        else if (c == '"') ss << "\\\"";
        else if (c == '\\') ss << "\\\\";
        else if (c < 32 || c >= 127) {
            char buf[8];
            snprintf(buf, sizeof buf, "\\%03o", c);
            ss << buf;
        } else ss << c;
    }
    return ss.str();
}

bool Arm64Emitter::exportEntry(const Function* f) const {
    if (!f || !f->isExport || f->isExtern || f->blocks.empty()) return false;
    bool rec = isCAggregate(f->returnType);
    for (const auto& p : f->params) rec = rec || isCAggregate(p.second) || isVaListType(p.second);
    return rec;
}

std::string Arm64Emitter::bodySym(const std::string& name) const {
    const Function* f = currentMod ? currentMod->getFunction(name) : nullptr;
    return mangleSym(exportEntry(f) ? name + ".esk" : name);
}

std::string Arm64Emitter::cabiSym(const std::string& name) const {
    const Function* f = currentMod ? currentMod->getFunction(name) : nullptr;
    return mangleSym(exportEntry(f) ? name : "__cabi_" + name);
}

std::string Arm64Emitter::mangleSym(const std::string& name) const {
    std::string out = isDarwin ? "_" : "";
    for (unsigned char c : name) {
        if (isalnum(c) || c == '_' || c == '.') out += static_cast<char>(c);
        else {
            char buf[8];
            snprintf(buf, sizeof buf, "$%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string Arm64Emitter::blockLabel(const std::string& bb) const {
    std::string b = bb;
    if (!b.empty() && b[0] == '$') b = b.substr(1);
    std::string out = localPrefix() + fnTag + "_";
    uint32_t h = 2166136261u;   // FNV-1a: the suffix is the same on every host
    for (unsigned char c : b) {
        out += (isalnum(c) || c == '_') ? static_cast<char>(c) : '_';
        h = (h ^ c) * 16777619u;
    }
    return out + "_" + std::to_string(h % 100000);
}

void Arm64Emitter::collectStrings(const Module* mod) {
    auto add = [&](const std::string& s) {
        if (stringTable.count(s)) return;
        std::string lbl = localPrefix() + "str_" + std::to_string(nextStrId++);
        stringTable[s] = lbl;
        stringOrder.push_back({s, lbl});
    };
    for (const auto& g : mod->globals) {
        if (g.init.kind == OperandKind::ConstantString) add(g.init.text);
        for (const auto& e : g.elements) {
            if (e.kind == OperandKind::ConstantString) add(e.text);
        }
    }
    for (const auto& fn : mod->functions) {
        for (const auto& bb : fn->blocks) {
            for (const auto& inst : bb->instructions) {
                for (const auto& op : inst->operands) {
                    if (op.kind == OperandKind::ConstantString) add(op.text);
                }
            }
        }
    }
}

size_t Arm64Emitter::sizeOf(const TypePtr& t) const {
    if (!t) return 8;
    size_t sz = 0;
    if (t->isStruct() && currentMod) sz = currentMod->getStructSize(t->nominalName());
    if (sz == 0) sz = getTypeSize(t, currentMod);
    if (sz == 0) sz = 8;
    return sz;
}

int64_t Arm64Emitter::truncConst(int64_t v, const TypePtr& t) {
    if (!t || !t->isInt()) return v;
    switch (t->bitWidth) {
        case 1: return v & 1;
        case 8: return t->isSigned ? static_cast<int64_t>(static_cast<int8_t>(v)) : static_cast<int64_t>(static_cast<uint8_t>(v));
        case 16: return t->isSigned ? static_cast<int64_t>(static_cast<int16_t>(v)) : static_cast<int64_t>(static_cast<uint16_t>(v));
        case 32: return t->isSigned ? static_cast<int64_t>(static_cast<int32_t>(v)) : static_cast<int64_t>(static_cast<uint32_t>(v));
        default: return v;
    }
}

void Arm64Emitter::emitGlobal(const GlobalVar& g, std::ostream& os) {
    std::string sym = mangleSym(g.name);
    bool local = g.name.find('.') != std::string::npos || isRuntimeName(g.name);
    if (!local) os << "    .globl " << sym << "\n";
    os << "    .p2align 3\n" << sym << ":\n";
    size_t sz = sizeOf(g.type);
    size_t written = 0;
    auto emitScalar = [&](const Operand& v, const TypePtr& ty) -> size_t {
        size_t esz = sizeOf(ty ? ty : Type::getPtr());
        if (v.kind == OperandKind::ConstantInt) {
            uint64_t u = static_cast<uint64_t>(v.intVal);
            if (esz == 1) os << "    .byte " << (u & 0xff) << "\n";
            else if (esz == 2) os << "    .short " << (u & 0xffff) << "\n";
            else if (esz == 4) os << "    .long " << (u & 0xffffffffu) << "\n";
            else if (esz == 8) os << "    .quad " << u << "\n";
            else os << "    .zero " << esz << "\n";
        } else if (v.kind == OperandKind::ConstantFloat) {
            if (esz == 4) {
                float f = static_cast<float>(v.floatVal);
                uint32_t bits;
                std::memcpy(&bits, &f, sizeof bits);
                os << "    .long " << bits << "\n";
            } else {
                uint64_t bits;
                std::memcpy(&bits, &v.floatVal, sizeof bits);
                os << "    .quad " << bits << "\n";
            }
        } else if (v.kind == OperandKind::ConstantString) {
            os << "    .quad " << stringTable[v.text] << "\n";
            esz = 8;
        } else if (v.kind == OperandKind::GlobalRef) {
            os << "    .quad " << bodySym(v.text) << "\n";
            esz = 8;
        } else {
            os << "    .zero " << esz << "\n";
        }
        return esz;
    };
    if (!g.elements.empty()) {
        for (const auto& e : g.elements) {
            if (written >= sz) break;
            written += emitScalar(e, e.type);
        }
    } else if (g.init.kind != OperandKind::ConstantNull && !isAggregate(g.type)) {
        TypePtr ty = g.type;
        if (g.init.kind == OperandKind::ConstantString || g.init.kind == OperandKind::GlobalRef) ty = Type::getPtr();
        written = emitScalar(g.init, ty);
    }
    if (written < sz) os << "    .zero " << (sz - written) << "\n";
}


std::unique_ptr<AsmEmitter> createArm64Emitter(const std::string& targetTriple) {
    return std::make_unique<Arm64Emitter>(targetTriple);
}

} // namespace eir
