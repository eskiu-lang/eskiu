#include "eir_asm.h"
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <algorithm>

namespace eir {

class Arm64Emitter : public AsmEmitter {
public:
    explicit Arm64Emitter(const std::string& triple) : targetTriple(triple) {
        isDarwin = targetTriple.find("darwin") != std::string::npos ||
                   targetTriple.find("apple") != std::string::npos ||
                   targetTriple.find("macos") != std::string::npos;
#if defined(__APPLE__)
        if (targetTriple.empty()) isDarwin = true;
#endif
    }

    void emitModule(const Module* mod, std::ostream& os) override {
        currentMod = mod;
        stringTable.clear();
        stringLabels.clear();

        // 1. Collect all string literals to emit in data / cstring section
        collectStrings(mod);

        // 2. Emit string constants
        if (!stringTable.empty()) {
            if (isDarwin) {
                os << "    .section __TEXT,__cstring,cstring_literals\n";
            } else {
                os << "    .section .rodata.str1.1,\"aMS\",@progbits,1\n";
            }
            for (const auto& kv : stringTable) {
                os << kv.second << ":\n";
                os << "    .asciz \"";
                for (char c : kv.first) {
                    if (c == '\n') os << "\\n";
                    else if (c == '\t') os << "\\t";
                    else if (c == '\r') os << "\\r";
                    else if (c == '\"') os << "\\\"";
                    else if (c == '\\') os << "\\\\";
                    else os << c;
                }
                os << "\"\n";
            }
            os << "\n";
        }

        // 3. Emit globals
        if (!mod->globals.empty()) {
            if (isDarwin) {
                os << "    .section __DATA,__data\n";
            } else {
                os << "    .data\n";
            }
            for (const auto& g : mod->globals) {
                std::string sym = mangleSym(g.name);
                os << "    .globl " << sym << "\n";
                os << "    .p2align 3\n";
                os << sym << ":\n";
                size_t sz = getTypeSize(g.type, mod);
                if (sz == 0) sz = 8;
                if (g.init.kind == OperandKind::ConstantInt) {
                    if (sz == 1) os << "    .byte " << g.init.intVal << "\n";
                    else if (sz == 2) os << "    .short " << g.init.intVal << "\n";
                    else if (sz == 4) os << "    .long " << g.init.intVal << "\n";
                    else os << "    .quad " << g.init.intVal << "\n";
                } else if (g.init.kind == OperandKind::ConstantString && stringTable.count(g.init.text)) {
                    os << "    .quad " << stringTable[g.init.text] << "\n";
                } else {
                    os << "    .zero " << sz << "\n";
                }
            }
            os << "\n";
        }

        // 4. Emit code section
        if (isDarwin) {
            os << "    .section __TEXT,__text,regular,pure_instructions\n";
        } else {
            os << "    .text\n";
        }

        for (const auto& fn : mod->functions) {
            if (!fn->isExtern) {
                emitFunction(fn.get(), os);
            }
        }
    }

private:
    std::string targetTriple;
    bool isDarwin = false;
    const Module* currentMod = nullptr;
    std::unordered_map<std::string, std::string> stringTable;
    std::vector<std::string> stringLabels;
    int nextStrId = 0;

    std::string mangleSym(const std::string& name) const {
        if (isDarwin) return "_" + name;
        return name;
    }

    void collectStrings(const Module* mod) {
        for (const auto& g : mod->globals) {
            if (g.init.kind == OperandKind::ConstantString && !stringTable.count(g.init.text)) {
                std::string lbl = ".L_str_" + std::to_string(nextStrId++);
                stringTable[g.init.text] = lbl;
            }
        }
        for (const auto& fn : mod->functions) {
            for (const auto& bb : fn->blocks) {
                for (const auto& inst : bb->instructions) {
                    for (const auto& op : inst->operands) {
                        if (op.kind == OperandKind::ConstantString && !stringTable.count(op.text)) {
                            std::string lbl = ".L_str_" + std::to_string(nextStrId++);
                            stringTable[op.text] = lbl;
                        }
                    }
                }
            }
        }
    }

    void emitFunction(const Function* fn, std::ostream& os) {
        std::string sym = mangleSym(fn->name);
        os << "    .globl " << sym << "\n";
        os << "    .p2align 2\n";
        os << sym << ":\n";

        // Stack frame calculation:
        std::unordered_map<std::string, size_t> slotOffsets;
        std::unordered_map<std::string, bool> isAllocaSlot;
        size_t currentOffset = 0;

        for (size_t i = 0; i < fn->params.size(); ++i) {
            size_t sz = 8;
            currentOffset += sz;
            slotOffsets["%" + fn->params[i].first] = currentOffset;
        }

        for (const auto& bb : fn->blocks) {
            for (const auto& inst : bb->instructions) {
                if (inst->op == Opcode::Alloca) {
                    size_t sz = getTypeSize(inst->type, currentMod);
                    if (sz == 0) sz = 8;
                    sz = (sz + 7) & ~7; // align to 8
                    currentOffset += sz;
                    slotOffsets[inst->resultVar] = currentOffset;
                    isAllocaSlot[inst->resultVar] = true;
                } else if (!inst->resultVar.empty() && !slotOffsets.count(inst->resultVar)) {
                    size_t sz = 8;
                    currentOffset += sz;
                    slotOffsets[inst->resultVar] = currentOffset;
                }
            }
        }

        // Align total stack frame to 16 bytes
        size_t frameSize = (currentOffset + 15) & ~15;
        if (frameSize == 0) frameSize = 16;

        // Prologue
        os << "    stp x29, x30, [sp, #-16]!\n";
        os << "    mov x29, sp\n";
        if (frameSize > 0) {
            if (frameSize <= 4095) {
                os << "    sub sp, sp, #" << frameSize << "\n";
            } else {
                os << "    mov x16, #" << frameSize << "\n";
                os << "    sub sp, sp, x16\n";
            }
        }

        // Store incoming arguments from registers into their stack slots
        for (size_t i = 0; i < fn->params.size() && i < 8; ++i) {
            size_t off = slotOffsets["%" + fn->params[i].first];
            os << "    str x" << i << ", [sp, #" << off << "]\n";
        }

        std::string retLabel = ".L_ret_" + fn->name;

        // Emit basic blocks
        for (const auto& bb : fn->blocks) {
            os << ".L_" << fn->name << "_" << bb->name << ":\n";
            for (const auto& inst : bb->instructions) {
                emitInstruction(fn, inst.get(), slotOffsets, isAllocaSlot, retLabel, os);
            }
        }

        // Epilogue
        os << retLabel << ":\n";
        os << "    mov sp, x29\n";
        os << "    ldp x29, x30, [sp], #16\n";
        os << "    ret\n\n";
    }

    void loadOperand(const Function* fn, const Operand& op, int regIdx,
                     const std::unordered_map<std::string, size_t>& slots,
                     const std::unordered_map<std::string, bool>& isAlloca,
                     std::ostream& os) {
        (void)fn;
        std::string reg = "x" + std::to_string(regIdx);

        switch (op.kind) {
            case OperandKind::ConstantInt: {
                int64_t v = op.intVal;
                if (v >= 0 && v <= 65535) {
                    os << "    mov " << reg << ", #" << v << "\n";
                } else if (v < 0 && v >= -65536) {
                    os << "    mov " << reg << ", #" << v << "\n";
                } else {
                    os << "    mov " << reg << ", #" << (v & 0xffff) << "\n";
                    if ((v >> 16) & 0xffff) os << "    movk " << reg << ", #" << ((v >> 16) & 0xffff) << ", lsl #16\n";
                    if ((v >> 32) & 0xffff) os << "    movk " << reg << ", #" << ((v >> 32) & 0xffff) << ", lsl #32\n";
                    if ((v >> 48) & 0xffff) os << "    movk " << reg << ", #" << ((v >> 48) & 0xffff) << ", lsl #48\n";
                }
                break;
            }
            case OperandKind::ConstantNull:
                os << "    mov " << reg << ", #0\n";
                break;
            case OperandKind::ConstantString: {
                std::string lbl = stringTable[op.text];
                if (isDarwin) {
                    os << "    adrp " << reg << ", " << lbl << "@PAGE\n";
                    os << "    add " << reg << ", " << reg << ", " << lbl << "@PAGEOFF\n";
                } else {
                    os << "    adrp " << reg << ", " << lbl << "\n";
                    os << "    add " << reg << ", " << reg << ", :lo12:" << lbl << "\n";
                }
                break;
            }
            case OperandKind::GlobalRef: {
                std::string sym = mangleSym(op.text);
                if (isDarwin) {
                    os << "    adrp " << reg << ", " << sym << "@PAGE\n";
                    os << "    add " << reg << ", " << reg << ", " << sym << "@PAGEOFF\n";
                } else {
                    os << "    adrp " << reg << ", " << sym << "\n";
                    os << "    add " << reg << ", " << reg << ", :lo12:" << sym << "\n";
                }
                break;
            }
            case OperandKind::Variable: {
                auto it = slots.find(op.text);
                if (it != slots.end()) {
                    auto aIt = isAlloca.find(op.text);
                    if (aIt != isAlloca.end() && aIt->second) {
                        // For an alloca variable used as value, its value is its stack address!
                        os << "    add " << reg << ", sp, #" << it->second << "\n";
                    } else {
                        os << "    ldr " << reg << ", [sp, #" << it->second << "]\n";
                    }
                } else {
                    os << "    mov " << reg << ", #0\n";
                }
                break;
            }
            default:
                os << "    mov " << reg << ", #0\n";
                break;
        }
    }

    void emitInstruction(const Function* fn, const Instruction* inst,
                         const std::unordered_map<std::string, size_t>& slots,
                         const std::unordered_map<std::string, bool>& isAlloca,
                         const std::string& retLabel,
                         std::ostream& os) {
        switch (inst->op) {
            case Opcode::Alloca:
                // Stack space was already reserved in prologue
                break;

            case Opcode::Store: {
                loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os);  // val in x9
                loadOperand(fn, inst->operands[1], 10, slots, isAlloca, os); // ptr in x10
                TypePtr ty = inst->operands[0].type;
                if (ty && ty->isInt() && ty->bitWidth == 8) {
                    os << "    strb w9, [x10]\n";
                } else if (ty && ty->isInt() && ty->bitWidth == 16) {
                    os << "    strh w9, [x10]\n";
                } else if (ty && ty->isInt() && ty->bitWidth == 32) {
                    os << "    str w9, [x10]\n";
                } else {
                    os << "    str x9, [x10]\n";
                }
                break;
            }

            case Opcode::Load: {
                loadOperand(fn, inst->operands[0], 10, slots, isAlloca, os); // ptr in x10
                TypePtr ty = inst->type;
                if (ty && ty->isInt() && ty->bitWidth == 8) {
                    os << (ty->isSigned ? "    ldrsb w9, [x10]\n" : "    ldrb w9, [x10]\n");
                } else if (ty && ty->isInt() && ty->bitWidth == 16) {
                    os << (ty->isSigned ? "    ldrsh w9, [x10]\n" : "    ldrh w9, [x10]\n");
                } else if (ty && ty->isInt() && ty->bitWidth == 32) {
                    os << "    ldr w9, [x10]\n";
                } else {
                    os << "    ldr x9, [x10]\n";
                }
                size_t off = slots.at(inst->resultVar);
                os << "    str x9, [sp, #" << off << "]\n";
                break;
            }

            case Opcode::Add:
            case Opcode::Sub:
            case Opcode::Mul:
            case Opcode::SDiv:
            case Opcode::UDiv: {
                loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os);
                loadOperand(fn, inst->operands[1], 10, slots, isAlloca, os);
                bool is32 = inst->type && inst->type->isInt() && inst->type->bitWidth <= 32;
                std::string rd = is32 ? "w9" : "x9";
                std::string rn = is32 ? "w9" : "x9";
                std::string rm = is32 ? "w10" : "x10";
                if (inst->op == Opcode::Add) os << "    add " << rd << ", " << rn << ", " << rm << "\n";
                else if (inst->op == Opcode::Sub) os << "    sub " << rd << ", " << rn << ", " << rm << "\n";
                else if (inst->op == Opcode::Mul) os << "    mul " << rd << ", " << rn << ", " << rm << "\n";
                else if (inst->op == Opcode::SDiv) os << "    sdiv " << rd << ", " << rn << ", " << rm << "\n";
                else if (inst->op == Opcode::UDiv) os << "    udiv " << rd << ", " << rn << ", " << rm << "\n";
                size_t off = slots.at(inst->resultVar);
                os << "    str x9, [sp, #" << off << "]\n";
                break;
            }

            case Opcode::ICmp: {
                loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os);
                loadOperand(fn, inst->operands[1], 10, slots, isAlloca, os);
                os << "    cmp x9, x10\n";
                std::string condStr = "eq";
                switch (inst->icmpCond) {
                    case ICmpCond::Eq: condStr = "eq"; break;
                    case ICmpCond::Ne: condStr = "ne"; break;
                    case ICmpCond::Slt: condStr = "lt"; break;
                    case ICmpCond::Sle: condStr = "le"; break;
                    case ICmpCond::Sgt: condStr = "gt"; break;
                    case ICmpCond::Sge: condStr = "ge"; break;
                    case ICmpCond::Ult: condStr = "lo"; break;
                    case ICmpCond::Ule: condStr = "ls"; break;
                    case ICmpCond::Ugt: condStr = "hi"; break;
                    case ICmpCond::Uge: condStr = "hs"; break;
                }
                os << "    cset x9, " << condStr << "\n";
                size_t off = slots.at(inst->resultVar);
                os << "    str x9, [sp, #" << off << "]\n";
                break;
            }

            case Opcode::Br: {
                std::string target = inst->operands[0].text;
                if (!target.empty() && target[0] == '$') target = target.substr(1);
                os << "    b .L_" << fn->name << "_" << target << "\n";
                break;
            }

            case Opcode::BrCond: {
                loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os);
                os << "    cmp x9, #0\n";
                std::string thenBB = inst->operands[1].text;
                if (!thenBB.empty() && thenBB[0] == '$') thenBB = thenBB.substr(1);
                std::string elseBB = inst->operands[2].text;
                if (!elseBB.empty() && elseBB[0] == '$') elseBB = elseBB.substr(1);
                os << "    b.ne .L_" << fn->name << "_" << thenBB << "\n";
                os << "    b .L_" << fn->name << "_" << elseBB << "\n";
                break;
            }

            case Opcode::Call: {
                std::string targetName = inst->operands[0].text;
                if (!targetName.empty() && targetName[0] == '@') targetName = targetName.substr(1);

                // Set up arguments (first 8 in x0-x7)
                for (size_t i = 1; i < inst->operands.size() && i <= 8; ++i) {
                    loadOperand(fn, inst->operands[i], static_cast<int>(i - 1), slots, isAlloca, os);
                }

                os << "    bl " << mangleSym(targetName) << "\n";

                if (!inst->resultVar.empty()) {
                    size_t off = slots.at(inst->resultVar);
                    os << "    str x0, [sp, #" << off << "]\n";
                }
                break;
            }

            case Opcode::Ret: {
                if (!inst->operands.empty()) {
                    loadOperand(fn, inst->operands[0], 0, slots, isAlloca, os);
                }
                os << "    b " << retLabel << "\n";
                break;
            }

            default:
                break;
        }
    }
};

std::unique_ptr<AsmEmitter> createArm64Emitter(const std::string& targetTriple) {
    return std::make_unique<Arm64Emitter>(targetTriple);
}

} // namespace eir
