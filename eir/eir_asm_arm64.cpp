#include "eir_asm.h"
#include "eir_runtime.h"
#include "../intrinsics.h"
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace eir {

namespace {

bool isAggregate(const TypePtr& t) {
    return t && (t->isStruct() || t->isArray() || t->isClosure() || t->isInterface() || t->isSlice());
}

bool isVaListType(const TypePtr& t) {
    return t && t->isStruct() && t->name == "__va_list";
}

// An aggregate an extern cannot take or return by value (a fn-typed parameter of an extern
// is a C function pointer, and a va_list is passed as C passes one).
bool isCAggregate(const TypePtr& t) {
    return isAggregate(t) && !t->isClosure() && !isVaListType(t);
}

bool isRuntimeName(const std::string& n) {
    return n.rfind("__eir_", 0) == 0 || n == "__exception_text_into";
}

size_t alignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

// Where one argument travels: an integer or FP register, or the stack at an offset from
// the outgoing area (caller) or from the incoming area (callee).
struct ArgLoc {
    bool fp = false;
    int reg = -1;
    size_t stackOff = 0;
    size_t size = 8;
    bool vararg = false;
};

} // namespace

// Values live in 8-byte frame slots; an integer narrower than 64 bits is kept extended
// to 64 by its signedness, so comparisons and address arithmetic use the full register.
// An aggregate (struct, array, closure, interface, slice) is the address of its bytes:
// a Load of one yields the address, a Store copies the bytes. A function returning an
// aggregate writes it to the buffer its caller passes in x8 and returns that address.
class Arm64Emitter : public AsmEmitter {
public:
    explicit Arm64Emitter(const std::string& triple) : targetTriple(triple) {
        isDarwin = targetTriple.find("darwin") != std::string::npos ||
                   targetTriple.find("apple") != std::string::npos ||
                   targetTriple.find("macos") != std::string::npos ||
                   targetTriple.find("ios") != std::string::npos;
#if defined(__APPLE__)
        if (targetTriple.empty()) isDarwin = true;
#endif
    }

    void emitModule(const Module* mod, std::ostream& os) override {
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
        int idx = 0;
        std::unordered_set<std::string> emitted;
        for (const auto& fn : mod->functions) {
            if (fn->isExtern || fn->blocks.empty()) continue;
            if (!emitted.insert(fn->name).second) continue;
            emitFunction(fn.get(), idx++, os);
        }
    }

private:
    std::string targetTriple;
    bool isDarwin = false;
    bool throws = false;
    const Module* currentMod = nullptr;
    std::unordered_map<std::string, std::string> stringTable;
    std::vector<std::pair<std::string, std::string>> stringOrder;
    std::unordered_set<std::string> definedSyms;
    int nextStrId = 0;
    int nextLocal = 0;

    // Per-function state
    const Function* curFn = nullptr;
    std::string fnTag;
    std::unordered_map<std::string, size_t> slots;
    std::unordered_set<std::string> addrVars;
    std::unordered_map<std::string, TypePtr> resultTypes;   // integer results narrower than 64 bits
    std::unordered_map<std::string, int> handlerIds;
    size_t sretSlot = 0, handlerDepthSlot = 0, handlerStackSlot = 0, handlerCap = 0;
    size_t caughtSlot = 0, retSaveSlot = 0, incomingStackBytes = 0;
    size_t grSaveSlot = 0, vrSaveSlot = 0;
    int fixedGr = 0, fixedVr = 0;
    bool fnHasSret = false, fnSavesCaught = false;

    std::string localPrefix() const { return isDarwin ? "L" : ".L"; }
    std::string newLocal(const std::string& what) { return localPrefix() + what + "_" + std::to_string(nextLocal++); }

    static std::string escapeAsm(const std::string& s) {
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

    // A symbol name: characters an assembler identifier cannot hold are spelled `$XX`.
    std::string mangleSym(const std::string& name) const {
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

    std::string blockLabel(const std::string& bb) const {
        std::string b = bb;
        if (!b.empty() && b[0] == '$') b = b.substr(1);
        std::string out = localPrefix() + fnTag + "_";
        for (unsigned char c : b) out += (isalnum(c) || c == '_') ? static_cast<char>(c) : '_';
        return out + "_" + std::to_string(std::hash<std::string>{}(b) % 100000);
    }

    void collectStrings(const Module* mod) {
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

    size_t sizeOf(const TypePtr& t) const {
        if (!t) return 8;
        size_t sz = 0;
        if (t->isStruct() && currentMod) sz = currentMod->getStructSize(t->nominalName());
        if (sz == 0) sz = getTypeSize(t, currentMod);
        if (sz == 0) sz = 8;
        return sz;
    }

    static int64_t truncConst(int64_t v, const TypePtr& t) {
        if (!t || !t->isInt()) return v;
        switch (t->bitWidth) {
            case 1: return v & 1;
            case 8: return t->isSigned ? static_cast<int64_t>(static_cast<int8_t>(v)) : static_cast<int64_t>(static_cast<uint8_t>(v));
            case 16: return t->isSigned ? static_cast<int64_t>(static_cast<int16_t>(v)) : static_cast<int64_t>(static_cast<uint16_t>(v));
            case 32: return t->isSigned ? static_cast<int64_t>(static_cast<int32_t>(v)) : static_cast<int64_t>(static_cast<uint32_t>(v));
            default: return v;
        }
    }

    void emitGlobal(const GlobalVar& g, std::ostream& os) {
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
                os << "    .quad " << mangleSym(v.text) << "\n";
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

    // ---- Immediates and frame addressing ----

    void movImm(const std::string& reg, int64_t v, std::ostream& os) {
        uint64_t uv = static_cast<uint64_t>(v);
        if (v >= 0 && v <= 65535) { os << "    mov " << reg << ", #" << v << "\n"; return; }
        if (v < 0 && v >= -65536) { os << "    mov " << reg << ", #" << v << "\n"; return; }
        os << "    movz " << reg << ", #" << (uv & 0xffff) << "\n";
        for (int sh = 16; sh < 64; sh += 16) {
            uint64_t part = (uv >> sh) & 0xffff;
            if (part) os << "    movk " << reg << ", #" << part << ", lsl #" << sh << "\n";
        }
    }

    void addImm(const std::string& rd, const std::string& rn, size_t imm, std::ostream& os) {
        if (imm <= 4095) {
            if (imm == 0 && rd == rn) return;
            os << "    add " << rd << ", " << rn << ", #" << imm << "\n";
        } else {
            movImm("x16", static_cast<int64_t>(imm), os);
            os << "    add " << rd << ", " << rn << ", x16\n";
        }
    }

    // `op reg, [base, #off]` with the scaled unsigned offset form when it encodes, else the
    // offset in x16.
    void memOp(const std::string& op, const std::string& reg, const std::string& base, size_t off,
               size_t scale, std::ostream& os) {
        if (off % scale == 0 && off / scale <= 4095) {
            os << "    " << op << " " << reg << ", [" << base << ", #" << off << "]\n";
        } else {
            movImm("x16", static_cast<int64_t>(off), os);
            os << "    " << op << " " << reg << ", [" << base << ", x16]\n";
        }
    }

    size_t slotOf(const std::string& v) const {
        auto it = slots.find(v);
        if (it == slots.end()) throw std::runtime_error("EIR arm64: no slot for " + v + " in " + curFn->name);
        return it->second;
    }

    // Store an instruction's result: an integer is first normalized to its type (every
    // integer value is held wrapped to its width and extended by its signedness).
    void storeX(const std::string& reg, const std::string& var, std::ostream& os) {
        auto it = resultTypes.find(var);
        if (it != resultTypes.end() && reg.size() > 1 && reg[0] == 'x' && isdigit(static_cast<unsigned char>(reg[1])))
            normalize(std::stoi(reg.substr(1)), it->second, os);
        memOp("str", reg, "sp", slotOf(var), 8, os);
    }

    void storeFloat(int fr, bool is32, const std::string& var, std::ostream& os) {
        if (is32) memOp("str", "s" + std::to_string(fr), "sp", slotOf(var), 4, os);
        else memOp("str", "d" + std::to_string(fr), "sp", slotOf(var), 8, os);
    }

    // Extend the low bits of x<r> to 64 by the type's width and signedness.
    void normalize(int r, const TypePtr& t, std::ostream& os) {
        if (!t || !t->isInt()) return;
        std::string x = "x" + std::to_string(r), w = "w" + std::to_string(r);
        switch (t->bitWidth) {
            case 1: os << "    and " << x << ", " << x << ", #1\n"; break;
            case 8: os << (t->isSigned ? "    sxtb " + x + ", " + w + "\n" : "    and " + x + ", " + x + ", #0xff\n"); break;
            case 16: os << (t->isSigned ? "    sxth " + x + ", " + w + "\n" : "    and " + x + ", " + x + ", #0xffff\n"); break;
            case 32: os << (t->isSigned ? "    sxtw " + x + ", " + w + "\n" : "    mov " + w + ", " + w + "\n"); break;
            default: break;
        }
    }

    void loadAddrOfSym(const std::string& reg, const std::string& name, std::ostream& os) {
        std::string sym = mangleSym(name);
        bool local = definedSyms.count(name) > 0;
        if (isDarwin) {
            if (local) {
                os << "    adrp " << reg << ", " << sym << "@PAGE\n";
                os << "    add " << reg << ", " << reg << ", " << sym << "@PAGEOFF\n";
            } else {
                os << "    adrp " << reg << ", " << sym << "@GOTPAGE\n";
                os << "    ldr " << reg << ", [" << reg << ", " << sym << "@GOTPAGEOFF]\n";
            }
        } else {
            if (local) {
                os << "    adrp " << reg << ", " << sym << "\n";
                os << "    add " << reg << ", " << reg << ", :lo12:" << sym << "\n";
            } else {
                os << "    adrp " << reg << ", :got:" << sym << "\n";
                os << "    ldr " << reg << ", [" << reg << ", :got_lo12:" << sym << "]\n";
            }
        }
    }

    void loadOperand(const Operand& op, int regIdx, std::ostream& os) {
        std::string reg = "x" + std::to_string(regIdx);
        switch (op.kind) {
            case OperandKind::ConstantInt:
                movImm(reg, truncConst(op.intVal, op.type), os);
                break;
            case OperandKind::ConstantNull:
                os << "    mov " << reg << ", #0\n";
                break;
            case OperandKind::ConstantFloat: {
                if (op.type && op.type->bitWidth == 32) {
                    float f = static_cast<float>(op.floatVal);
                    uint32_t bits;
                    std::memcpy(&bits, &f, sizeof bits);
                    movImm(reg, bits, os);
                } else {
                    uint64_t bits;
                    std::memcpy(&bits, &op.floatVal, sizeof bits);
                    movImm(reg, static_cast<int64_t>(bits), os);
                }
                break;
            }
            case OperandKind::ConstantString: {
                const std::string& lbl = stringTable[op.text];
                if (isDarwin) {
                    os << "    adrp " << reg << ", " << lbl << "@PAGE\n";
                    os << "    add " << reg << ", " << reg << ", " << lbl << "@PAGEOFF\n";
                } else {
                    os << "    adrp " << reg << ", " << lbl << "\n";
                    os << "    add " << reg << ", " << reg << ", :lo12:" << lbl << "\n";
                }
                break;
            }
            case OperandKind::GlobalRef:
                loadAddrOfSym(reg, op.text, os);
                break;
            case OperandKind::Variable: {
                // A value nothing defined (a void result) reads as 0, as in the EVM.
                if (!slots.count(op.text)) os << "    mov " << reg << ", #0\n";
                else if (addrVars.count(op.text)) addImm(reg, "sp", slotOf(op.text), os);
                else memOp("ldr", reg, "sp", slotOf(op.text), 8, os);
                break;
            }
            default:
                throw std::runtime_error("EIR arm64: unsupported operand " + op.str());
        }
    }

    // Load a value into s<r>/d<r> as the width `want32` asks, converting from the
    // operand's own type (an integer converts by its signedness).
    void loadFloat(const Operand& op, int r, bool want32, std::ostream& os) {
        std::string s = "s" + std::to_string(r), d = "d" + std::to_string(r);
        std::string dst = want32 ? s : d;
        if (op.kind == OperandKind::ConstantFloat) {
            if (want32) {
                float f = static_cast<float>(op.floatVal);
                uint32_t bits;
                std::memcpy(&bits, &f, sizeof bits);
                movImm("w16", bits, os);
                os << "    fmov " << s << ", w16\n";
            } else {
                uint64_t bits;
                std::memcpy(&bits, &op.floatVal, sizeof bits);
                movImm("x16", static_cast<int64_t>(bits), os);
                os << "    fmov " << d << ", x16\n";
            }
            return;
        }
        if (op.kind == OperandKind::Variable && op.type && op.type->isFloat() && slots.count(op.text)) {
            bool src32 = op.type->bitWidth == 32;
            if (src32) memOp("ldr", s, "sp", slotOf(op.text), 4, os);
            else memOp("ldr", d, "sp", slotOf(op.text), 8, os);
            if (src32 && !want32) os << "    fcvt " << d << ", " << s << "\n";
            if (!src32 && want32) os << "    fcvt " << s << ", " << d << "\n";
            return;
        }
        loadOperand(op, 16, os);
        bool uns = op.type && op.type->isInt() && !op.type->isSigned;
        os << "    " << (uns ? "ucvtf " : "scvtf ") << dst << ", x16\n";
    }

    // ---- Calling convention ----

    size_t argSize(const TypePtr& t) const {
        if (!t || isAggregate(t)) return 8;
        if (t->isFloat()) return t->bitWidth == 32 ? 4 : 8;
        if (t->isInt()) return t->bitWidth <= 8 ? 1 : t->bitWidth <= 16 ? 2 : t->bitWidth <= 32 ? 4 : 8;
        return 8;
    }

    std::vector<ArgLoc> classify(const std::vector<TypePtr>& tys, size_t fixedCount, bool variadic, size_t* stackBytes) const {
        std::vector<ArgLoc> locs;
        int ngr = 0, nvr = 0;
        size_t off = 0;
        for (size_t i = 0; i < tys.size(); ++i) {
            ArgLoc l;
            l.fp = tys[i] && tys[i]->isFloat();
            l.size = argSize(tys[i]);
            l.vararg = variadic && i >= fixedCount;
            if (isDarwin && l.vararg) {
                off = alignUp(off, 8);
                l.stackOff = off;
                l.size = 8;
                off += 8;
            } else if (l.fp && nvr < 8) {
                l.reg = nvr++;
            } else if (!l.fp && ngr < 8) {
                l.reg = ngr++;
            } else {
                size_t al = isDarwin ? l.size : 8;
                off = alignUp(off, al);
                l.stackOff = off;
                off += isDarwin ? l.size : 8;
            }
            locs.push_back(l);
        }
        if (stackBytes) *stackBytes = alignUp(off, 16);
        return locs;
    }

    struct CallShape {
        std::vector<TypePtr> argTypes;
        size_t fixedCount = 0;
        bool variadic = false;
        bool external = false;
        bool indirectEnv = false;
    };

    CallShape callShape(const Instruction* inst) const {
        CallShape cs;
        const Function* callee = nullptr;
        if (inst->op == Opcode::Call) {
            callee = currentMod->getFunction(inst->operands[0].text);
            cs.external = !callee || callee->isExtern || callee->blocks.empty();
        } else {
            cs.indirectEnv = inst->operands[0].type && inst->operands[0].type->isClosure();
        }
        if (cs.indirectEnv) cs.argTypes.push_back(Type::getPtr());
        for (size_t i = 1; i < inst->operands.size(); ++i) {
            TypePtr t = inst->operands[i].type;
            if (callee && i - 1 < callee->params.size() && callee->params[i - 1].second) t = callee->params[i - 1].second;
            if (inst->operands[i].kind == OperandKind::ConstantFloat && !(t && t->isFloat())) t = Type::getF64();
            cs.argTypes.push_back(t);
        }
        cs.variadic = callee && callee->isVariadic;
        cs.fixedCount = callee ? callee->params.size() : cs.argTypes.size();
        return cs;
    }

    static bool isSpecialCall(const std::string& n) {
        return n == "va_start" || n == "va_end" || n == "va_arg" || n == "llvm.va_start.p0" || n == "llvm.va_end.p0";
    }

    // ---- Functions ----

    void emitFunction(const Function* fn, int idx, std::ostream& os) {
        curFn = fn;
        fnTag = "F" + std::to_string(idx);
        slots.clear();
        addrVars.clear();
        resultTypes.clear();
        handlerIds.clear();

        fnHasSret = isAggregate(fn->returnType);
        fnSavesCaught = false;
        size_t pushCount = 0;
        size_t outgoing = 0;

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
                if ((inst->op == Opcode::Call || inst->op == Opcode::CallIndirect) && !isSpecialCall(inst->operands[0].text)) {
                    size_t bytes = 0;
                    CallShape cs = callShape(inst.get());
                    classify(cs.argTypes, cs.fixedCount, cs.variadic, &bytes);
                    outgoing = std::max(outgoing, bytes);
                }
            }
        }

        size_t local = outgoing;
        auto alloc = [&](size_t sz, size_t al) {
            local = alignUp(local, std::max<size_t>(al, 8));
            size_t off = local;
            local += alignUp(std::max<size_t>(sz, 1), 8);
            return off;
        };

        std::vector<TypePtr> paramTypes;
        for (const auto& p : fn->params) {
            slots["%" + p.first] = alloc(8, 8);
            paramTypes.push_back(p.second);
        }
        std::vector<ArgLoc> paramLocs = classify(paramTypes, paramTypes.size(), false, &incomingStackBytes);
        incomingStackBytes = 0;
        fixedGr = fixedVr = 0;
        for (const auto& l : paramLocs) {
            if (l.reg < 0) incomingStackBytes = std::max(incomingStackBytes, alignUp(l.stackOff + l.size, 8));
            else if (l.fp) fixedVr++;
            else fixedGr++;
        }
        if (fnHasSret) sretSlot = alloc(8, 8);
        if (fn->isVariadic && !isDarwin) {
            grSaveSlot = alloc(64, 16);
            vrSaveSlot = alloc(128, 16);
        }
        if (pushCount > 0) {
            handlerCap = std::max<size_t>(16, pushCount * 4);
            handlerDepthSlot = alloc(8, 8);
            handlerStackSlot = alloc(handlerCap * 8, 8);
        }
        if (fnSavesCaught) {
            caughtSlot = alloc(8, 8);
            retSaveSlot = alloc(16, 8);
        }

        for (const auto& bb : fn->blocks) {
            for (const auto& inst : bb->instructions) {
                const std::string& rv = inst->resultVar;
                if (inst->op == Opcode::Alloca) {
                    size_t al = inst->operands.empty() ? 8 : static_cast<size_t>(std::max<int64_t>(inst->operands[0].intVal, 1));
                    if (!slots.count(rv)) slots[rv] = alloc(sizeOf(inst->type), std::min<size_t>(al, 16));
                    addrVars.insert(rv);
                } else if (inst->op == Opcode::MakeClosure || inst->op == Opcode::MakeInterface || inst->op == Opcode::MakeSlice) {
                    if (!slots.count(rv)) slots[rv] = alloc(16, 8);
                    addrVars.insert(rv);
                } else if ((inst->op == Opcode::Call || inst->op == Opcode::CallIndirect) && !rv.empty() && isAggregate(inst->type)) {
                    if (!slots.count(rv)) slots[rv] = alloc(sizeOf(inst->type), 8);
                    addrVars.insert(rv);
                } else if (inst->op == Opcode::InlineAsm && !rv.empty() && inst->type && inst->type->isStruct()) {
                    if (!slots.count(rv)) slots[rv] = alloc(8 * std::max<size_t>(inst->type->members.size(), 1), 8);
                    addrVars.insert(rv);
                } else if (!rv.empty() && !slots.count(rv)) {
                    slots[rv] = alloc(8, 8);
                }
                if (!rv.empty() && !addrVars.count(rv) && inst->type && inst->type->isInt() && inst->type->bitWidth < 64)
                    resultTypes[rv] = inst->type;
            }
        }

        size_t frameSize = alignUp(local, 16);
        if (frameSize == 0) frameSize = 16;

        std::string sym = mangleSym(fn->name);
        if (!isRuntimeName(fn->name)) os << "    .globl " << sym << "\n";
        os << "    .p2align 2\n" << sym << ":\n";
        os << "    stp x29, x30, [sp, #-16]!\n";
        os << "    mov x29, sp\n";
        if (frameSize <= 4095) {
            os << "    sub sp, sp, #" << frameSize << "\n";
        } else {
            movImm("x16", static_cast<int64_t>(frameSize), os);
            os << "    sub sp, sp, x16\n";
        }

        if (fn->isVariadic && !isDarwin) {
            addImm("x16", "sp", grSaveSlot, os);
            for (int r = 0; r < 8; r += 2)
                os << "    stp x" << r << ", x" << r + 1 << ", [x16, #" << r * 8 << "]\n";
            addImm("x16", "sp", vrSaveSlot, os);
            for (int r = 0; r < 8; r += 2)
                os << "    stp q" << r << ", q" << r + 1 << ", [x16, #" << r * 16 << "]\n";
        }
        if (fnHasSret) memOp("str", "x8", "sp", sretSlot, 8, os);

        for (size_t i = 0; i < fn->params.size(); ++i) {
            const ArgLoc& l = paramLocs[i];
            TypePtr pty = fn->params[i].second;
            std::string var = "%" + fn->params[i].first;
            if (l.fp) {
                bool is32 = pty->bitWidth == 32;
                if (l.reg >= 0) {
                    storeFloat(l.reg, is32, var, os);
                } else {
                    memOp("ldr", is32 ? "s16" : "d16", "x29", 16 + l.stackOff, is32 ? 4 : 8, os);
                    storeFloat(16, is32, var, os);
                }
                continue;
            }
            if (l.reg >= 0) {
                if (l.reg != 9) os << "    mov x9, x" << l.reg << "\n";
            } else {
                loadSized("x9", "x29", 16 + l.stackOff, isDarwin ? pty : Type::getI64(), os);
            }
            normalize(9, pty, os);
            storeX("x9", var, os);
        }

        if (pushCount > 0) memOp("str", "xzr", "sp", handlerDepthSlot, 8, os);
        if (fnSavesCaught) {
            os << "    bl " << mangleSym("__eir_caught_depth") << "\n";
            memOp("str", "x0", "sp", caughtSlot, 8, os);
        }

        std::string retLabel = localPrefix() + "ret_" + fnTag;
        std::string unwindLabel = localPrefix() + "unwind_" + fnTag;

        for (const auto& bb : fn->blocks) {
            os << blockLabel(bb->name) << ":\n";
            for (const auto& inst : bb->instructions) emitInstruction(inst.get(), retLabel, unwindLabel, os);
            if (!bb->hasTerminator()) {
                if (fn->name == "main") os << "    mov x0, #0\n";
                os << "    b " << retLabel << "\n";
            }
        }

        if (throws || pushCount > 0) {
            os << unwindLabel << ":\n";
            if (pushCount > 0) {
                std::string none = newLocal("nohandler");
                memOp("ldr", "x9", "sp", handlerDepthSlot, 8, os);
                os << "    cbz x9, " << none << "\n";
                os << "    sub x9, x9, #1\n";
                memOp("str", "x9", "sp", handlerDepthSlot, 8, os);
                addImm("x10", "sp", handlerStackSlot, os);
                os << "    ldr x9, [x10, x9, lsl #3]\n";
                for (const auto& kv : handlerIds) {
                    movImm("x10", kv.second, os);
                    os << "    cmp x9, x10\n";
                    os << "    b.eq " << blockLabel(kv.first) << "\n";
                }
                os << "    brk #1\n";
                os << none << ":\n";
            }
            if (fn->name == "main") os << "    bl " << mangleSym("__eir_uncaught") << "\n";
            os << "    mov x0, #0\n";
            os << "    b " << retLabel << "\n";
        }

        os << retLabel << ":\n";
        if (fnSavesCaught) {
            memOp("str", "x0", "sp", retSaveSlot, 8, os);
            memOp("str", "d0", "sp", retSaveSlot + 8, 8, os);
            memOp("ldr", "x0", "sp", caughtSlot, 8, os);
            os << "    bl " << mangleSym("__eir_set_caught_depth") << "\n";
            memOp("ldr", "x0", "sp", retSaveSlot, 8, os);
            memOp("ldr", "d0", "sp", retSaveSlot + 8, 8, os);
        }
        os << "    mov sp, x29\n";
        os << "    ldp x29, x30, [sp], #16\n";
        os << "    ret\n\n";
    }

    // Load a value of type t from [base + off] into `reg` (x form), extended by its type.
    void loadSized(const std::string& xreg, const std::string& base, size_t off, const TypePtr& t, std::ostream& os) {
        std::string w = "w" + xreg.substr(1);
        if (!t || !t->isInt() || t->bitWidth > 32) { memOp("ldr", xreg, base, off, 8, os); return; }
        if (t->bitWidth <= 8) memOp(t->isSigned && t->bitWidth > 1 ? "ldrsb" : "ldrb", t->isSigned && t->bitWidth > 1 ? xreg : w, base, off, 1, os);
        else if (t->bitWidth <= 16) memOp(t->isSigned ? "ldrsh" : "ldrh", t->isSigned ? xreg : w, base, off, 2, os);
        else memOp(t->isSigned ? "ldrsw" : "ldr", t->isSigned ? xreg : w, base, off, 4, os);
    }

    // Copy `size` bytes from [x9] to [x10] (clobbers x9-x13).
    void copyBytes(size_t size, std::ostream& os) {
        if (size <= 128) {
            size_t b = 0;
            for (; b + 8 <= size; b += 8) {
                os << "    ldr x11, [x9, #" << b << "]\n";
                os << "    str x11, [x10, #" << b << "]\n";
            }
            for (; b < size; ++b) {
                os << "    ldrb w11, [x9, #" << b << "]\n";
                os << "    strb w11, [x10, #" << b << "]\n";
            }
            return;
        }
        std::string loop = newLocal("copy");
        movImm("x12", static_cast<int64_t>(size / 8), os);
        os << loop << ":\n";
        os << "    ldr x11, [x9], #8\n";
        os << "    str x11, [x10], #8\n";
        os << "    subs x12, x12, #1\n";
        os << "    b.ne " << loop << "\n";
        for (size_t b = 0; b < size % 8; ++b) {
            os << "    ldrb w11, [x9], #1\n";
            os << "    strb w11, [x10], #1\n";
        }
    }

    void divCheck(const Operand& divisor, std::ostream& os) {
        if (divisor.kind == OperandKind::ConstantInt && truncConst(divisor.intVal, divisor.type) != 0) return;
        std::string ok = newLocal("divok");
        os << "    cbnz x10, " << ok << "\n";
        os << "    brk #1\n";
        os << ok << ":\n";
    }

    void exceptionCheck(std::ostream& os, const std::string& unwindLabel) {
        if (!throws) return;
        os << "    bl " << mangleSym("__eir_exc_pending") << "\n";
        os << "    cbnz x0, " << unwindLabel << "\n";
    }

    // ---- Instructions ----

    void emitInstruction(const Instruction* inst, const std::string& retLabel,
                         const std::string& unwindLabel, std::ostream& os) {
        const auto& ops = inst->operands;
        switch (inst->op) {
            // A stack slot starts zeroed each time its alloca runs, as in the EVM.
            case Opcode::Alloca: {
                size_t sz = alignUp(sizeOf(inst->type), 8);
                addImm("x10", "sp", slotOf(inst->resultVar), os);
                if (sz <= 128) {
                    for (size_t b = 0; b < sz; b += 8) os << "    str xzr, [x10, #" << b << "]\n";
                } else {
                    std::string loop = newLocal("zero");
                    movImm("x12", static_cast<int64_t>(sz / 8), os);
                    os << loop << ":\n    str xzr, [x10], #8\n    subs x12, x12, #1\n    b.ne " << loop << "\n";
                }
                break;
            }

            case Opcode::Store: {
                TypePtr valTy = ops[0].type;
                TypePtr dstTy = (ops[1].type && ops[1].type->pointee) ? ops[1].type->pointee : valTy;
                if (!dstTy) dstTy = valTy;
                if (isAggregate(dstTy) || (!dstTy && isAggregate(valTy))) {
                    loadOperand(ops[0], 9, os);
                    loadOperand(ops[1], 10, os);
                    copyBytes(sizeOf(dstTy ? dstTy : valTy), os);
                    break;
                }
                loadOperand(ops[1], 10, os);
                if (dstTy && dstTy->isFloat()) {
                    bool is32 = dstTy->bitWidth == 32;
                    loadFloat(ops[0], 16, is32, os);
                    os << "    str " << (is32 ? "s16" : "d16") << ", [x10]\n";
                    break;
                }
                bool valFloat = (valTy && valTy->isFloat()) || ops[0].kind == OperandKind::ConstantFloat;
                if (valFloat && dstTy && dstTy->isInt()) {
                    loadFloat(ops[0], 16, false, os);
                    os << "    fcvtzs x9, d16\n";
                } else {
                    loadOperand(ops[0], 9, os);
                }
                size_t w = (dstTy && dstTy->isInt()) ? getTypeSize(dstTy) : 8;
                if (w == 1) os << "    strb w9, [x10]\n";
                else if (w == 2) os << "    strh w9, [x10]\n";
                else if (w == 4) os << "    str w9, [x10]\n";
                else os << "    str x9, [x10]\n";
                break;
            }

            case Opcode::Load: {
                loadOperand(ops[0], 10, os);
                TypePtr ty = inst->type;
                if (isAggregate(ty)) {
                    storeX("x10", inst->resultVar, os);
                } else if (ty && ty->isFloat()) {
                    bool is32 = ty->bitWidth == 32;
                    os << "    ldr " << (is32 ? "s16" : "d16") << ", [x10]\n";
                    storeFloat(16, is32, inst->resultVar, os);
                } else {
                    loadSized("x9", "x10", 0, ty, os);
                    storeX("x9", inst->resultVar, os);
                }
                break;
            }

            case Opcode::FieldPtr: {
                std::string structName = ops[0].text;
                loadOperand(ops[1], 10, os);
                std::string fieldName = ops[2].text;
                size_t off = currentMod->getFieldOffset(structName, fieldName);
                if (off == 0 && (structName.empty() || structName == "slice")) {
                    if (fieldName == "len") off = 8;
                }
                addImm("x10", "x10", off, os);
                storeX("x10", inst->resultVar, os);
                break;
            }

            case Opcode::OffsetPtr: {
                loadOperand(ops[0], 9, os);
                loadOperand(ops[1], 10, os);
                os << "    add x9, x9, x10\n";
                storeX("x9", inst->resultVar, os);
                break;
            }

            case Opcode::Gep: {
                loadOperand(ops[0], 9, os);
                loadOperand(ops[1], 10, os);
                TypePtr elemType = inst->type->pointee ? inst->type->pointee : (inst->type->elemType ? inst->type->elemType : inst->type);
                size_t elemSize = sizeOf(elemType);
                if (elemSize == 1) os << "    add x9, x9, x10\n";
                else if (elemSize == 2) os << "    add x9, x9, x10, lsl #1\n";
                else if (elemSize == 4) os << "    add x9, x9, x10, lsl #2\n";
                else if (elemSize == 8) os << "    add x9, x9, x10, lsl #3\n";
                else {
                    movImm("x11", static_cast<int64_t>(elemSize), os);
                    os << "    madd x9, x10, x11, x9\n";
                }
                storeX("x9", inst->resultVar, os);
                break;
            }

            // Integer values follow the EVM: 64-bit arithmetic, wrapped to 32 bits for a
            // 32-bit add/sub/mul, a narrow unsigned bitwise result masked to its width,
            // shifts done at the type's width with the count masked.
            case Opcode::Add: case Opcode::Sub: case Opcode::Mul: {
                loadOperand(ops[0], 9, os);
                loadOperand(ops[1], 10, os);
                bool is32 = inst->type && inst->type->isInt() && inst->type->bitWidth == 32;
                const char* mn = inst->op == Opcode::Add ? "add" : inst->op == Opcode::Sub ? "sub" : "mul";
                if (is32) {
                    os << "    " << mn << " w9, w9, w10\n";
                    normalize(9, inst->type, os);
                } else {
                    os << "    " << mn << " x9, x9, x10\n";
                }
                storeX("x9", inst->resultVar, os);
                break;
            }

            case Opcode::And: case Opcode::Or: case Opcode::Xor: {
                loadOperand(ops[0], 9, os);
                loadOperand(ops[1], 10, os);
                const char* mn = inst->op == Opcode::And ? "and" : inst->op == Opcode::Or ? "orr" : "eor";
                os << "    " << mn << " x9, x9, x10\n";
                if (inst->type && inst->type->isInt() && !inst->type->isSigned && inst->type->bitWidth > 1 && inst->type->bitWidth < 64)
                    normalize(9, inst->type, os);
                storeX("x9", inst->resultVar, os);
                break;
            }

            case Opcode::Shl: case Opcode::LShr: case Opcode::AShr: {
                loadOperand(ops[0], 9, os);
                loadOperand(ops[1], 10, os);
                unsigned bw = (inst->type && inst->type->isInt() && inst->type->bitWidth > 1) ? inst->type->bitWidth : 64;
                if (bw < 32) {
                    os << "    and x10, x10, #" << (bw - 1) << "\n";
                    if (inst->op == Opcode::AShr) os << "    sbfx x9, x9, #0, #" << bw << "\n";
                    else os << "    ubfx x9, x9, #0, #" << bw << "\n";
                    const char* mn = inst->op == Opcode::Shl ? "lsl" : inst->op == Opcode::LShr ? "lsr" : "asr";
                    os << "    " << mn << " x9, x9, x10\n";
                    if (inst->op == Opcode::AShr) os << "    sbfx x9, x9, #0, #" << bw << "\n";
                    else os << "    ubfx x9, x9, #0, #" << bw << "\n";
                } else if (bw == 32) {
                    const char* mn = inst->op == Opcode::Shl ? "lsl" : inst->op == Opcode::LShr ? "lsr" : "asr";
                    os << "    " << mn << " w9, w9, w10\n";
                    if (inst->op == Opcode::LShr) os << "    mov w9, w9\n";
                    else if (inst->op == Opcode::AShr) os << "    sxtw x9, w9\n";
                    else normalize(9, inst->type, os);
                } else {
                    const char* mn = inst->op == Opcode::Shl ? "lsl" : inst->op == Opcode::LShr ? "lsr" : "asr";
                    os << "    " << mn << " x9, x9, x10\n";
                }
                storeX("x9", inst->resultVar, os);
                break;
            }

            case Opcode::SDiv: case Opcode::UDiv: case Opcode::SRem: case Opcode::URem: {
                loadOperand(ops[0], 9, os);
                loadOperand(ops[1], 10, os);
                divCheck(ops[1], os);
                bool sgn = inst->op == Opcode::SDiv || inst->op == Opcode::SRem;
                bool is32 = sgn && inst->type && inst->type->isInt() && inst->type->bitWidth == 32;
                std::string r = is32 ? "w" : "x";
                os << "    " << (sgn ? "sdiv " : "udiv ") << r << "11, " << r << "9, " << r << "10\n";
                if (inst->op == Opcode::SRem || inst->op == Opcode::URem)
                    os << "    msub " << r << "9, " << r << "11, " << r << "10, " << r << "9\n";
                else
                    os << "    mov " << r << "9, " << r << "11\n";
                if (is32) os << "    sxtw x9, w9\n";
                storeX("x9", inst->resultVar, os);
                break;
            }

            case Opcode::FAdd: case Opcode::FSub: case Opcode::FMul: case Opcode::FDiv: {
                bool is32 = inst->type && inst->type->bitWidth == 32;
                loadFloat(ops[0], 16, is32, os);
                loadFloat(ops[1], 17, is32, os);
                std::string a = is32 ? "s16" : "d16", b = is32 ? "s17" : "d17";
                const char* mn = inst->op == Opcode::FAdd ? "fadd" : inst->op == Opcode::FSub ? "fsub"
                               : inst->op == Opcode::FMul ? "fmul" : "fdiv";
                os << "    " << mn << " " << a << ", " << a << ", " << b << "\n";
                storeFloat(16, is32, inst->resultVar, os);
                break;
            }

            case Opcode::Not: {
                loadOperand(ops[0], 9, os);
                os << "    mvn x9, x9\n";
                storeX("x9", inst->resultVar, os);
                break;
            }

            case Opcode::Neg: {
                if (inst->type && inst->type->isFloat()) {
                    bool is32 = inst->type->bitWidth == 32;
                    loadFloat(ops[0], 16, is32, os);
                    os << "    fneg " << (is32 ? "s16, s16" : "d16, d16") << "\n";
                    storeFloat(16, is32, inst->resultVar, os);
                } else {
                    loadOperand(ops[0], 9, os);
                    os << "    neg x9, x9\n";
                    storeX("x9", inst->resultVar, os);
                }
                break;
            }

            case Opcode::ICmp: {
                loadOperand(ops[0], 9, os);
                loadOperand(ops[1], 10, os);
                if (ops[0].type && ops[0].type->isInterface()) os << "    cbz x9, " << "1f\n    ldr x9, [x9]\n1:\n";
                if (ops[1].type && ops[1].type->isInterface()) os << "    cbz x10, " << "1f\n    ldr x10, [x10]\n1:\n";
                os << "    cmp x9, x10\n";
                const char* c = "eq";
                switch (inst->icmpCond) {
                    case ICmpCond::Eq: c = "eq"; break;
                    case ICmpCond::Ne: c = "ne"; break;
                    case ICmpCond::Slt: c = "lt"; break;
                    case ICmpCond::Sle: c = "le"; break;
                    case ICmpCond::Sgt: c = "gt"; break;
                    case ICmpCond::Sge: c = "ge"; break;
                    case ICmpCond::Ult: c = "lo"; break;
                    case ICmpCond::Ule: c = "ls"; break;
                    case ICmpCond::Ugt: c = "hi"; break;
                    case ICmpCond::Uge: c = "hs"; break;
                }
                os << "    cset x9, " << c << "\n";
                storeX("x9", inst->resultVar, os);
                break;
            }

            case Opcode::FCmp: {
                bool is32 = (ops[0].type && ops[0].type->isFloat() && ops[0].type->bitWidth == 32) &&
                            (!ops[1].type || !ops[1].type->isFloat() || ops[1].type->bitWidth == 32);
                loadFloat(ops[0], 16, is32, os);
                loadFloat(ops[1], 17, is32, os);
                os << "    fcmp " << (is32 ? "s16, s17" : "d16, d17") << "\n";
                const char* c = "eq";
                switch (inst->fcmpCond) {
                    case FCmpCond::Oeq: c = "eq"; break;
                    case FCmpCond::One: c = "ne"; break;
                    case FCmpCond::Olt: c = "mi"; break;
                    case FCmpCond::Ole: c = "ls"; break;
                    case FCmpCond::Ogt: c = "gt"; break;
                    case FCmpCond::Oge: c = "ge"; break;
                }
                os << "    cset x9, " << c << "\n";
                storeX("x9", inst->resultVar, os);
                break;
            }

            case Opcode::SIToFP: {
                bool is32 = inst->type && inst->type->bitWidth == 32;
                loadOperand(ops[0], 9, os);
                bool uns = ops[0].type && ops[0].type->isInt() && !ops[0].type->isSigned;
                os << "    " << (uns ? "ucvtf " : "scvtf ") << (is32 ? "s16" : "d16") << ", x9\n";
                storeFloat(16, is32, inst->resultVar, os);
                break;
            }

            case Opcode::FPToSI: {
                bool src32 = ops[0].type && ops[0].type->isFloat() && ops[0].type->bitWidth == 32;
                loadFloat(ops[0], 16, src32, os);
                TypePtr dt = inst->type;
                bool uns = dt && dt->isInt() && !dt->isSigned;
                unsigned bw = dt && dt->isInt() ? dt->bitWidth : 32;
                std::string src = src32 ? "s16" : "d16";
                if (bw > 32) {
                    os << "    " << (uns ? "fcvtzu" : "fcvtzs") << " x9, " << src << "\n";
                } else {
                    os << "    " << (uns ? "fcvtzu" : "fcvtzs") << " w9, " << src << "\n";
                    if (bw < 32 && bw > 1) {
                        int64_t hi = uns ? ((1LL << bw) - 1) : ((1LL << (bw - 1)) - 1);
                        movImm("w10", hi, os);
                        os << "    cmp w9, w10\n";
                        os << "    csel w9, w10, w9, " << (uns ? "hi" : "gt") << "\n";
                        if (!uns) {
                            movImm("w10", -(1LL << (bw - 1)), os);
                            os << "    cmp w9, w10\n";
                            os << "    csel w9, w10, w9, lt\n";
                        }
                    }
                }
                normalize(9, dt, os);
                storeX("x9", inst->resultVar, os);
                break;
            }

            case Opcode::FPTrunc: {
                loadFloat(ops[0], 16, true, os);
                storeFloat(16, true, inst->resultVar, os);
                break;
            }

            case Opcode::FPExt: {
                loadFloat(ops[0], 16, false, os);
                storeFloat(16, false, inst->resultVar, os);
                break;
            }

            case Opcode::Trunc: {
                loadOperand(ops[0], 9, os);
                normalize(9, inst->type, os);
                storeX("x9", inst->resultVar, os);
                break;
            }

            case Opcode::SExt:
            case Opcode::ZExt: {
                loadOperand(ops[0], 9, os);
                unsigned bw = ops[0].type && ops[0].type->isInt() ? ops[0].type->bitWidth : 64;
                if (bw < 64) {
                    if (inst->op == Opcode::SExt) os << "    sbfx x9, x9, #0, #" << bw << "\n";
                    else os << "    ubfx x9, x9, #0, #" << bw << "\n";
                }
                normalize(9, inst->type, os);
                storeX("x9", inst->resultVar, os);
                break;
            }

            case Opcode::Bitcast:
            case Opcode::PtrToInt:
            case Opcode::IntToPtr: {
                bool srcF = ops[0].type && ops[0].type->isFloat();
                bool dstF = inst->type && inst->type->isFloat();
                if (srcF && dstF) {
                    bool is32 = inst->type->bitWidth == 32;
                    loadFloat(ops[0], 16, is32, os);
                    storeFloat(16, is32, inst->resultVar, os);
                } else if (srcF) {
                    bool is32 = ops[0].type->bitWidth == 32;
                    loadFloat(ops[0], 16, is32, os);
                    os << "    fmov " << (is32 ? "w9, s16" : "x9, d16") << "\n";
                    normalize(9, inst->type, os);
                    storeX("x9", inst->resultVar, os);
                } else if (dstF) {
                    loadOperand(ops[0], 9, os);
                    bool is32 = inst->type->bitWidth == 32;
                    os << "    fmov " << (is32 ? "s16, w9" : "d16, x9") << "\n";
                    storeFloat(16, is32, inst->resultVar, os);
                } else {
                    loadOperand(ops[0], 9, os);
                    normalize(9, inst->type, os);
                    storeX("x9", inst->resultVar, os);
                }
                break;
            }

            case Opcode::Br:
                os << "    b " << blockLabel(ops[0].text) << "\n";
                break;

            case Opcode::BrCond:
                loadOperand(ops[0], 9, os);
                os << "    cbnz x9, " << blockLabel(ops[1].text) << "\n";
                os << "    b " << blockLabel(ops[2].text) << "\n";
                break;

            case Opcode::Switch: {
                loadOperand(ops[0], 9, os);
                for (const auto& sc : inst->switchCases) {
                    movImm("x10", sc.first, os);
                    os << "    cmp x9, x10\n";
                    os << "    b.eq " << blockLabel(sc.second) << "\n";
                }
                if (!inst->defaultLabel.empty()) os << "    b " << blockLabel(inst->defaultLabel) << "\n";
                break;
            }

            case Opcode::Call:
            case Opcode::CallIndirect:
                emitCall(inst, unwindLabel, os);
                break;

            case Opcode::MakeClosure:
            case Opcode::MakeInterface:
            case Opcode::MakeSlice: {
                size_t slot = slotOf(inst->resultVar);
                loadOperand(ops[0], 9, os);
                memOp("str", "x9", "sp", slot, 8, os);
                loadOperand(ops[1], 9, os);
                memOp("str", "x9", "sp", slot + 8, 8, os);
                break;
            }

            case Opcode::ExtractFat: {
                loadOperand(ops[0], 10, os);
                int64_t idx = ops[1].kind == OperandKind::ConstantInt ? ops[1].intVal : 0;
                if (inst->type && inst->type->isFloat()) {
                    bool is32 = inst->type->bitWidth == 32;
                    memOp("ldr", is32 ? "s16" : "d16", "x10", static_cast<size_t>(idx) * 8, is32 ? 4 : 8, os);
                    storeFloat(16, is32, inst->resultVar, os);
                } else {
                    memOp("ldr", "x9", "x10", static_cast<size_t>(idx) * 8, 8, os);
                    normalize(9, inst->type, os);
                    storeX("x9", inst->resultVar, os);
                }
                break;
            }

            case Opcode::Ret: {
                bool hasVal = !ops.empty() && ops[0].type && !ops[0].type->isVoid();
                if (fnHasSret) {
                    memOp("ldr", "x10", "sp", sretSlot, 8, os);
                    if (hasVal) {
                        loadOperand(ops[0], 9, os);
                        copyBytes(sizeOf(curFn->returnType), os);
                    }
                    memOp("ldr", "x0", "sp", sretSlot, 8, os);
                } else if (hasVal && curFn->returnType && curFn->returnType->isFloat()) {
                    loadFloat(ops[0], 0, curFn->returnType->bitWidth == 32, os);
                } else if (hasVal) {
                    loadOperand(ops[0], 0, os);
                } else if (curFn->name == "main") {
                    os << "    mov x0, #0\n";
                }
                os << "    b " << retLabel << "\n";
                break;
            }

            case Opcode::Unreachable:
                os << "    brk #1\n";
                break;

            case Opcode::Throw:
                loadOperand(ops[0], 0, os);
                os << "    bl " << mangleSym("__eir_throw") << "\n";
                os << "    b " << unwindLabel << "\n";
                break;

            case Opcode::LandingPad:
                os << "    bl " << mangleSym("__eir_take_exception") << "\n";
                if (!inst->resultVar.empty()) storeX("x0", inst->resultVar, os);
                break;

            case Opcode::PushHandler: {
                int id = handlerIds.at(ops[0].text);
                std::string ok = newLocal("hok");
                memOp("ldr", "x9", "sp", handlerDepthSlot, 8, os);
                movImm("x10", static_cast<int64_t>(handlerCap), os);
                os << "    cmp x9, x10\n";
                os << "    b.lo " << ok << "\n";
                os << "    brk #1\n";
                os << ok << ":\n";
                addImm("x10", "sp", handlerStackSlot, os);
                movImm("x11", id, os);
                os << "    str x11, [x10, x9, lsl #3]\n";
                os << "    add x9, x9, #1\n";
                memOp("str", "x9", "sp", handlerDepthSlot, 8, os);
                break;
            }

            case Opcode::PopHandler: {
                if (handlerCap == 0) break;
                memOp("ldr", "x9", "sp", handlerDepthSlot, 8, os);
                os << "    subs x9, x9, #1\n";
                os << "    csel x9, xzr, x9, lt\n";
                memOp("str", "x9", "sp", handlerDepthSlot, 8, os);
                break;
            }

            case Opcode::InlineAsm:
                emitInlineAsm(inst, os);
                break;

            default:
                throw std::runtime_error("EIR arm64: unsupported instruction " + inst->str());
        }
    }

    // ---- Calls ----

    void emitVaStart(const Operand& ap, std::ostream& os) {
        loadOperand(ap, 10, os);
        addImm("x9", "x29", 16 + incomingStackBytes, os);
        os << "    str x9, [x10]\n";
        if (isDarwin) return;
        addImm("x9", "sp", grSaveSlot + 64, os);
        os << "    str x9, [x10, #8]\n";
        addImm("x9", "sp", vrSaveSlot + 128, os);
        os << "    str x9, [x10, #16]\n";
        movImm("w9", -(8 - fixedGr) * 8, os);
        os << "    str w9, [x10, #24]\n";
        movImm("w9", -(8 - fixedVr) * 16, os);
        os << "    str w9, [x10, #28]\n";
    }

    // x9 <- address of the next argument of type t, advancing the va_list at [x10].
    void emitVaArgAddr(const TypePtr& t, std::ostream& os) {
        bool fp = t && t->isFloat();
        if (isDarwin) {
            os << "    ldr x9, [x10]\n";
            os << "    add x11, x9, #8\n";
            os << "    str x11, [x10]\n";
            return;
        }
        std::string stack = newLocal("vastack"), done = newLocal("vadone");
        size_t offsField = fp ? 28 : 24, topField = fp ? 16 : 8, step = fp ? 16 : 8;
        os << "    ldrsw x11, [x10, #" << offsField << "]\n";
        os << "    tbz x11, #63, " << stack << "\n";
        os << "    add x12, x11, #" << step << "\n";
        os << "    str w12, [x10, #" << offsField << "]\n";
        os << "    cmp x12, #0\n";
        os << "    b.gt " << stack << "\n";
        os << "    ldr x9, [x10, #" << topField << "]\n";
        os << "    add x9, x9, x11\n";
        os << "    b " << done << "\n";
        os << stack << ":\n";
        os << "    ldr x9, [x10]\n";
        os << "    add x11, x9, #8\n";
        os << "    str x11, [x10]\n";
        os << done << ":\n";
    }

    bool emitSpecialCall(const Instruction* inst, std::ostream& os) {
        const std::string& n = inst->operands[0].text;
        if (currentMod->getFunction(n) && !currentMod->getFunction(n)->isExtern) return false;
        if (n == "va_start" || n == "llvm.va_start.p0") {
            if (!curFn->isVariadic) throw std::runtime_error("EIR arm64: va_start outside a variadic function");
            emitVaStart(inst->operands[1], os);
            return true;
        }
        if (n == "va_end" || n == "llvm.va_end.p0") return true;
        if (n == "va_arg") {
            TypePtr t = inst->type;
            if (isAggregate(t)) throw std::runtime_error("EIR arm64: va_arg of an aggregate is not supported");
            loadOperand(inst->operands[1], 10, os);
            emitVaArgAddr(t, os);
            if (inst->resultVar.empty()) return true;
            if (t && t->isFloat()) {
                os << "    ldr d16, [x9]\n";
                if (t->bitWidth == 32) os << "    fcvt s16, d16\n";
                storeFloat(16, t->bitWidth == 32, inst->resultVar, os);
            } else {
                os << "    ldr x9, [x9]\n";
                normalize(9, t, os);
                storeX("x9", inst->resultVar, os);
            }
            return true;
        }
        return false;
    }

    // An `intrinsic` atomic, inline: acquire/release exclusives (plain ones when relaxed),
    // the cell's width by its kind.
    bool emitAtomic(const Instruction* inst, std::ostream& os) {
        const std::string& n = inst->operands[0].text;
        AtomicIntrinsic a;
        if (!parseAtomicIntrinsic(n, a) || currentMod->getFunction(n)) return false;
        if (a.op == "fence") {
            os << "    dmb ish\n";
            return true;
        }
        std::string r = a.kind == AtomicKind::I32 ? "w" : "x";
        loadOperand(inst->operands[1], 9, os);
        if (inst->operands.size() > 2) loadOperand(inst->operands[2], 10, os);
        if (inst->operands.size() > 3) loadOperand(inst->operands[3], 13, os);
        std::string ld = a.relaxed ? "ldxr" : "ldaxr", st = a.relaxed ? "stxr" : "stlxr";
        if (a.op == "load") {
            os << "    " << (a.relaxed ? "ldr " : "ldar ") << r << "11, [x9]\n";
        } else if (a.op == "store") {
            os << "    " << (a.relaxed ? "str " : "stlr ") << r << "10, [x9]\n";
        } else if (a.op == "cas") {
            std::string loop = newLocal("cas"), fail = newLocal("casfail"), done = newLocal("casdone");
            os << loop << ":\n";
            os << "    " << ld << " " << r << "11, [x9]\n";
            os << "    cmp " << r << "11, " << r << "10\n";
            os << "    b.ne " << fail << "\n";
            os << "    " << st << " w12, " << r << "13, [x9]\n";
            os << "    cbnz w12, " << loop << "\n";
            os << "    mov x11, #1\n";
            os << "    b " << done << "\n";
            os << fail << ":\n";
            os << "    clrex\n";
            os << "    mov x11, #0\n";
            os << done << ":\n";
        } else {
            std::string loop = newLocal("rmw");
            const char* opc = a.op == "fetch_add" ? "add" : a.op == "fetch_sub" ? "sub" : a.op == "fetch_and" ? "and"
                            : a.op == "fetch_or" ? "orr" : a.op == "fetch_xor" ? "eor" : nullptr;
            os << loop << ":\n";
            os << "    " << ld << " " << r << "11, [x9]\n";
            if (opc) os << "    " << opc << " " << r << "13, " << r << "11, " << r << "10\n";
            else os << "    mov " << r << "13, " << r << "10\n";
            os << "    " << st << " w12, " << r << "13, [x9]\n";
            os << "    cbnz w12, " << loop << "\n";
        }
        if (!inst->resultVar.empty()) {
            if (a.kind == AtomicKind::I32 && a.op != "cas") os << "    sxtw x11, w11\n";
            storeX("x11", inst->resultVar, os);
        }
        return true;
    }

    void emitCall(const Instruction* inst, const std::string& unwindLabel, std::ostream& os) {
        if (inst->op == Opcode::Call && isSpecialCall(inst->operands[0].text) && emitSpecialCall(inst, os)) return;
        if (inst->op == Opcode::Call && emitAtomic(inst, os)) return;

        CallShape cs = callShape(inst);
        const std::string& target = inst->operands[0].text;
        if (cs.external && inst->op == Opcode::Call && !isRuntimeName(target)) {
            for (size_t i = 0; i < cs.argTypes.size(); ++i) {
                if (isCAggregate(cs.argTypes[i]))
                    throw std::runtime_error("EIR arm64: passing an aggregate by value to extern '" + target + "' is not supported");
            }
            if (isCAggregate(inst->type))
                throw std::runtime_error("EIR arm64: extern '" + target + "' returns an aggregate by value, which is not supported");
        }

        std::vector<ArgLoc> locs = classify(cs.argTypes, cs.fixedCount, cs.variadic, nullptr);
        std::vector<Operand> args;
        if (cs.indirectEnv) args.push_back(Operand::constNull());
        for (size_t i = 1; i < inst->operands.size(); ++i) args.push_back(inst->operands[i]);

        // Stack arguments first: their loads use only x9/x16/v16.
        for (size_t i = 0; i < args.size(); ++i) {
            const ArgLoc& l = locs[i];
            if (l.reg >= 0) continue;
            if (cs.indirectEnv && i == 0) continue;
            TypePtr t = cs.argTypes[i];
            if (l.fp || (l.vararg && args[i].kind == OperandKind::ConstantFloat)) {
                bool is32 = !l.vararg && t && t->bitWidth == 32;
                loadFloat(args[i], 16, is32, os);
                memOp("str", is32 ? "s16" : "d16", "sp", l.stackOff, is32 ? 4 : 8, os);
                continue;
            }
            loadArgInt(args[i], t, cs.external, 9, os);
            size_t w = (isDarwin && !l.vararg) ? l.size : 8;
            if (w == 1) memOp("strb", "w9", "sp", l.stackOff, 1, os);
            else if (w == 2) memOp("strh", "w9", "sp", l.stackOff, 2, os);
            else if (w == 4) memOp("str", "w9", "sp", l.stackOff, 4, os);
            else memOp("str", "x9", "sp", l.stackOff, 8, os);
        }

        for (size_t i = 0; i < args.size(); ++i) {
            const ArgLoc& l = locs[i];
            if (l.reg < 0 || (cs.indirectEnv && i == 0)) continue;
            if (l.fp) {
                loadFloat(args[i], l.reg, !l.vararg && cs.argTypes[i] && cs.argTypes[i]->bitWidth == 32, os);
            } else {
                loadArgInt(args[i], cs.argTypes[i], cs.external, l.reg, os);
            }
        }

        if (inst->op == Opcode::CallIndirect) {
            loadOperand(inst->operands[0], 17, os);
            if (cs.indirectEnv) {
                os << "    ldr x0, [x17, #8]\n";
                os << "    ldr x17, [x17]\n";
            }
        }
        if (isAggregate(inst->type) && !inst->resultVar.empty()) addImm("x8", "sp", slotOf(inst->resultVar), os);

        if (inst->op == Opcode::CallIndirect) os << "    blr x17\n";
        else os << "    bl " << mangleSym(target) << "\n";

        if (!inst->resultVar.empty() && !isAggregate(inst->type)) {
            if (inst->type && inst->type->isFloat()) {
                storeFloat(0, inst->type->bitWidth == 32, inst->resultVar, os);
            } else {
                normalize(0, inst->type, os);
                storeX("x0", inst->resultVar, os);
            }
        }

        bool mayThrow = inst->op == Opcode::CallIndirect || (!cs.external && !isRuntimeName(target));
        if (mayThrow) exceptionCheck(os, unwindLabel);
    }

    void loadArgInt(const Operand& op, const TypePtr& t, bool external, int reg, std::ostream& os) {
        if (external && isVaListType(t)) {
            loadOperand(op, reg, os);
            if (isDarwin) os << "    ldr x" << reg << ", [x" << reg << "]\n";
            return;
        }
        loadOperand(op, reg, os);
    }

    // ---- Inline assembly ----

    void emitInlineAsm(const Instruction* inst, std::ostream& os) {
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
        struct Bound { std::string text; int reg = -1; bool regOut = false; TypePtr ty; };
        std::vector<Bound> bound;
        std::vector<int> outRegs;
        std::vector<TypePtr> outTypes;
        std::vector<TypePtr> resultTypes;
        if (inst->type && inst->type->isStruct()) resultTypes = inst->type->members;
        else if (inst->type && !inst->type->isVoid()) resultTypes.push_back(inst->type);

        static const int pool[] = {9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7};
        size_t nextReg = 0, opIdx = 0, outIdx = 0;
        auto take = [&]() {
            if (nextReg >= sizeof(pool) / sizeof(pool[0])) throw std::runtime_error("EIR arm64: too many asm operands");
            return pool[nextReg++];
        };
        auto regName = [](int r, const TypePtr& t) {
            bool narrow = t && t->isInt() && t->bitWidth <= 32;
            return std::string(narrow ? "w" : "x") + std::to_string(r);
        };
        std::ostringstream loads;
        for (const auto& c : cons) {
            if (c.rfind("~{", 0) == 0) continue;
            Bound b;
            if (c == "=*m" || c == "*m" || c == "m") {
                if (opIdx >= inst->operands.size()) throw std::runtime_error("EIR arm64: asm operand mismatch");
                b.reg = take();
                loadOperand(inst->operands[opIdx++], b.reg, loads);
                b.text = "[x" + std::to_string(b.reg) + "]";
            } else if (!c.empty() && c[0] == '=') {
                b.reg = take();
                b.regOut = true;
                b.ty = outIdx < resultTypes.size() ? resultTypes[outIdx] : Type::getI64();
                outIdx++;
                outRegs.push_back(b.reg);
                outTypes.push_back(b.ty);
                b.text = regName(b.reg, b.ty);
            } else if (!c.empty() && isdigit(static_cast<unsigned char>(c[0]))) {
                size_t tiedTo = static_cast<size_t>(std::stoi(c));
                if (tiedTo >= bound.size() || opIdx >= inst->operands.size()) throw std::runtime_error("EIR arm64: asm tie mismatch");
                b = bound[tiedTo];
                b.regOut = false;
                loadOperand(inst->operands[opIdx++], b.reg, loads);
            } else if (c == "i" || c == "n") {
                const Operand& o = inst->operands[opIdx++];
                b.text = "#" + std::to_string(o.intVal);
            } else {
                if (opIdx >= inst->operands.size()) throw std::runtime_error("EIR arm64: asm operand mismatch");
                const Operand& o = inst->operands[opIdx++];
                b.reg = take();
                b.ty = o.type;
                loadOperand(o, b.reg, loads);
                b.text = regName(b.reg, o.type);
            }
            bound.push_back(b);
        }
        os << loads.str();

        std::string text;
        const std::string& a = inst->asmString;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i] == '$' && i + 1 < a.size() && a[i + 1] == '$') { text += '$'; ++i; continue; }
            if (a[i] == '$' && i + 1 < a.size() && (isdigit(static_cast<unsigned char>(a[i + 1])) || a[i + 1] == '{')) {
                size_t j = i + 1;
                bool brace = a[j] == '{';
                if (brace) ++j;
                size_t start = j;
                while (j < a.size() && isdigit(static_cast<unsigned char>(a[j]))) ++j;
                size_t n = static_cast<size_t>(std::stoul(a.substr(start, j - start)));
                std::string mod;
                if (brace) {
                    if (j < a.size() && a[j] == ':') {
                        size_t e = a.find('}', j);
                        mod = a.substr(j + 1, e - j - 1);
                        j = e;
                    }
                    ++j;
                }
                if (n >= bound.size()) throw std::runtime_error("EIR arm64: asm operand $" + std::to_string(n) + " out of range");
                const Bound& b = bound[n];
                if (!mod.empty() && b.reg >= 0 && (mod == "w" || mod == "x")) text += mod + std::to_string(b.reg);
                else text += b.text;
                i = j - 1;
                continue;
            }
            text += a[i];
        }
        os << "    " << text << "\n";

        if (outRegs.size() == 1 && !inst->resultVar.empty() && !addrVars.count(inst->resultVar)) {
            if (outRegs[0] != 9) os << "    mov x9, x" << outRegs[0] << "\n";
            normalize(9, outTypes[0], os);
            storeX("x9", inst->resultVar, os);
        } else if (!outRegs.empty() && !inst->resultVar.empty()) {
            size_t base = slotOf(inst->resultVar);
            for (size_t k = 0; k < outRegs.size(); ++k) {
                normalize(outRegs[k], outTypes[k], os);
                memOp("str", "x" + std::to_string(outRegs[k]), "sp", base + k * 8, 8, os);
            }
        }
    }
};

std::unique_ptr<AsmEmitter> createArm64Emitter(const std::string& targetTriple) {
    return std::make_unique<Arm64Emitter>(targetTriple);
}

} // namespace eir
