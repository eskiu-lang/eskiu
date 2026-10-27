#include "eir_asm.h"
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <cstring>

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
                } else if (g.init.kind == OperandKind::ConstantFloat) {
                    if (sz == 4) {
                        float f = static_cast<float>(g.init.floatVal);
                        uint32_t bits;
                        std::memcpy(&bits, &f, sizeof(bits));
                        os << "    .long " << bits << "\n";
                    } else {
                        uint64_t bits;
                        std::memcpy(&bits, &g.init.floatVal, sizeof(bits));
                        os << "    .quad " << bits << "\n";
                    }
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

    void emitAddImm(const std::string& rd, const std::string& rn, size_t imm, std::ostream& os) {
        if (imm == 0 && rd == rn) return;
        if (imm <= 4095) {
            os << "    add " << rd << ", " << rn << ", #" << imm << "\n";
        } else {
            os << "    mov x16, #" << imm << "\n";
            os << "    add " << rd << ", " << rn << ", x16\n";
        }
    }

    void emitLdr(const std::string& reg, const std::string& base, size_t off, std::ostream& os) {
        if (off <= 32760 && (off % 8 == 0)) {
            os << "    ldr " << reg << ", [" << base << ", #" << off << "]\n";
        } else {
            os << "    mov x16, #" << off << "\n";
            os << "    ldr " << reg << ", [" << base << ", x16]\n";
        }
    }

    void emitStr(const std::string& reg, const std::string& base, size_t off, std::ostream& os) {
        if (off <= 32760 && (off % 8 == 0)) {
            os << "    str " << reg << ", [" << base << ", #" << off << "]\n";
        } else {
            os << "    mov x16, #" << off << "\n";
            os << "    str " << reg << ", [" << base << ", x16]\n";
        }
    }

    void emitLdr32(const std::string& reg, const std::string& base, size_t off, std::ostream& os) {
        if (off <= 16380 && (off % 4 == 0)) {
            os << "    ldr " << reg << ", [" << base << ", #" << off << "]\n";
        } else {
            os << "    mov x16, #" << off << "\n";
            os << "    ldr " << reg << ", [" << base << ", x16]\n";
        }
    }

    void emitStr32(const std::string& reg, const std::string& base, size_t off, std::ostream& os) {
        if (off <= 16380 && (off % 4 == 0)) {
            os << "    str " << reg << ", [" << base << ", #" << off << "]\n";
        } else {
            os << "    mov x16, #" << off << "\n";
            os << "    str " << reg << ", [" << base << ", x16]\n";
        }
    }

    void emitIntImm(int64_t v, const std::string& reg, std::ostream& os) {
        if (v >= 0 && v <= 65535) {
            os << "    mov " << reg << ", #" << v << "\n";
        } else if (v < 0 && v >= -65536) {
            os << "    mov " << reg << ", #" << v << "\n";
        } else {
            uint64_t uv = static_cast<uint64_t>(v);
            os << "    mov " << reg << ", #" << (uv & 0xffff) << "\n";
            if ((uv >> 16) & 0xffff) os << "    movk " << reg << ", #" << ((uv >> 16) & 0xffff) << ", lsl #16\n";
            if ((uv >> 32) & 0xffff) os << "    movk " << reg << ", #" << ((uv >> 32) & 0xffff) << ", lsl #32\n";
            if ((uv >> 48) & 0xffff) os << "    movk " << reg << ", #" << ((uv >> 48) & 0xffff) << ", lsl #48\n";
        }
    }

    void emitFloatImm(double val, bool is32, int fpRegIdx, std::ostream& os) {
        if (is32) {
            float f = static_cast<float>(val);
            uint32_t bits;
            std::memcpy(&bits, &f, sizeof(bits));
            os << "    mov w16, #" << (bits & 0xffff) << "\n";
            if ((bits >> 16) & 0xffff) {
                os << "    movk w16, #" << ((bits >> 16) & 0xffff) << ", lsl #16\n";
            }
            os << "    fmov s" << fpRegIdx << ", w16\n";
        } else {
            uint64_t bits;
            std::memcpy(&bits, &val, sizeof(bits));
            os << "    mov x16, #" << (bits & 0xffff) << "\n";
            if ((bits >> 16) & 0xffff) os << "    movk x16, #" << ((bits >> 16) & 0xffff) << ", lsl #16\n";
            if ((bits >> 32) & 0xffff) os << "    movk x16, #" << ((bits >> 32) & 0xffff) << ", lsl #32\n";
            if ((bits >> 48) & 0xffff) os << "    movk x16, #" << ((bits >> 48) & 0xffff) << ", lsl #48\n";
            os << "    fmov d" << fpRegIdx << ", x16\n";
        }
    }

    void emitFunction(const Function* fn, std::ostream& os) {
        std::string sym = mangleSym(fn->name);
        os << "    .globl " << sym << "\n";
        os << "    .p2align 2\n";
        os << sym << ":\n";

        // 1. Calculate maxOutgoingBytes for call arguments
        size_t maxOutgoingBytes = 0;
        for (const auto& bb : fn->blocks) {
            for (const auto& inst : bb->instructions) {
                if (inst->op == Opcode::Call || inst->op == Opcode::CallIndirect) {
                    std::string targetName = inst->operands[0].text;
                    if (!targetName.empty() && targetName[0] == '@') targetName = targetName.substr(1);
                    const Function* targetFn = currentMod ? currentMod->getFunction(targetName) : nullptr;
                    bool isVar = targetFn ? targetFn->isVariadic : (targetName.find("printf") != std::string::npos);
                    size_t fixedCount = targetFn ? targetFn->params.size() : (targetName.find("printf") != std::string::npos ? 1 : 0);

                    size_t numStackArgs = 0;
                    if (isDarwin && isVar) {
                        if (inst->operands.size() > fixedCount + 1) {
                            numStackArgs = inst->operands.size() - 1 - fixedCount;
                        }
                        if (fixedCount > 8) numStackArgs += (fixedCount - 8);
                    } else {
                        size_t numInt = 0, numFp = 0;
                        for (size_t i = 1; i < inst->operands.size(); ++i) {
                            const auto& op = inst->operands[i];
                            if ((op.type && op.type->isFloat()) || op.kind == OperandKind::ConstantFloat) {
                                numFp++;
                            } else {
                                numInt++;
                            }
                        }
                        size_t spilledInt = numInt > 8 ? numInt - 8 : 0;
                        size_t spilledFp = numFp > 8 ? numFp - 8 : 0;
                        numStackArgs = spilledInt + spilledFp;
                    }
                    size_t needed = (numStackArgs * 8 + 15) & ~15;
                    if (needed > maxOutgoingBytes) maxOutgoingBytes = needed;
                }
            }
        }

        // 2. Allocate stack slots
        std::unordered_map<std::string, size_t> slotOffsets;
        std::unordered_map<std::string, bool> isAllocaSlot;
        size_t localOffset = 0;

        for (size_t i = 0; i < fn->params.size(); ++i) {
            size_t sz = 8;
            slotOffsets["%" + fn->params[i].first] = maxOutgoingBytes + localOffset;
            localOffset += sz;
        }

        for (const auto& bb : fn->blocks) {
            for (const auto& inst : bb->instructions) {
                if (inst->op == Opcode::Alloca) {
                    size_t sz = getTypeSize(inst->type, currentMod);
                    if (sz == 0 && inst->type && inst->type->isStruct() && currentMod) {
                        sz = currentMod->getStructSize(inst->type->nominalName());
                    }
                    if (sz == 0) sz = 8;
                    sz = (sz + 7) & ~7; // align to 8
                    slotOffsets[inst->resultVar] = maxOutgoingBytes + localOffset;
                    isAllocaSlot[inst->resultVar] = true;
                    localOffset += sz;
                } else if (inst->op == Opcode::MakeSlice || inst->op == Opcode::MakeInterface || inst->op == Opcode::MakeClosure) {
                    slotOffsets[inst->resultVar] = maxOutgoingBytes + localOffset;
                    isAllocaSlot[inst->resultVar] = true;
                    localOffset += 16;
                } else if (!inst->resultVar.empty() && !slotOffsets.count(inst->resultVar)) {
                    size_t sz = 8;
                    slotOffsets[inst->resultVar] = maxOutgoingBytes + localOffset;
                    localOffset += sz;
                }
            }
        }

        // Align total stack frame to 16 bytes
        size_t frameSize = (maxOutgoingBytes + localOffset + 15) & ~15;
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

        // Store incoming arguments into their stack slots
        int intParamIdx = 0;
        int fpParamIdx = 0;
        for (size_t i = 0; i < fn->params.size(); ++i) {
            size_t off = slotOffsets["%" + fn->params[i].first];
            TypePtr pty = fn->params[i].second;
            if (pty && pty->isFloat()) {
                if (fpParamIdx < 8) {
                    if (pty->bitWidth == 32) {
                        emitStr32("s" + std::to_string(fpParamIdx), "sp", off, os);
                    } else {
                        emitStr("d" + std::to_string(fpParamIdx), "sp", off, os);
                    }
                    fpParamIdx++;
                }
            } else {
                if (intParamIdx < 8) {
                    emitStr("x" + std::to_string(intParamIdx), "sp", off, os);
                    intParamIdx++;
                }
            }
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

    void loadFloatOperand(const Function* fn, const Operand& op, int fpRegIdx, bool is32,
                          const std::unordered_map<std::string, size_t>& slots,
                          std::ostream& os) {
        (void)fn;
        std::string freg = (is32 ? "s" : "d") + std::to_string(fpRegIdx);
        switch (op.kind) {
            case OperandKind::ConstantFloat:
                emitFloatImm(op.floatVal, is32, fpRegIdx, os);
                break;
            case OperandKind::ConstantInt:
                emitIntImm(op.intVal, "x16", os);
                if (is32) os << "    scvtf " << freg << ", w16\n";
                else os << "    scvtf " << freg << ", x16\n";
                break;
            case OperandKind::Variable: {
                auto it = slots.find(op.text);
                if (it != slots.end()) {
                    if (is32) emitLdr32(freg, "sp", it->second, os);
                    else emitLdr(freg, "sp", it->second, os);
                } else {
                    emitFloatImm(0.0, is32, fpRegIdx, os);
                }
                break;
            }
            default:
                emitFloatImm(0.0, is32, fpRegIdx, os);
                break;
        }
    }

    void loadOperand(const Function* fn, const Operand& op, int regIdx,
                     const std::unordered_map<std::string, size_t>& slots,
                     const std::unordered_map<std::string, bool>& isAlloca,
                     std::ostream& os) {
        (void)fn;
        std::string reg = "x" + std::to_string(regIdx);

        switch (op.kind) {
            case OperandKind::ConstantInt:
                emitIntImm(op.intVal, reg, os);
                break;
            case OperandKind::ConstantNull:
                os << "    mov " << reg << ", #0\n";
                break;
            case OperandKind::ConstantFloat: {
                uint64_t bits;
                std::memcpy(&bits, &op.floatVal, sizeof(bits));
                emitIntImm(static_cast<int64_t>(bits), reg, os);
                break;
            }
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
                        emitAddImm(reg, "sp", it->second, os);
                    } else {
                        emitLdr(reg, "sp", it->second, os);
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
                // Stack space reserved in prologue
                break;

            case Opcode::Store: {
                loadOperand(fn, inst->operands[1], 10, slots, isAlloca, os); // ptr in x10
                TypePtr ty = inst->operands[0].type;
                bool isFloat = (ty && ty->isFloat()) || (inst->operands[0].kind == OperandKind::ConstantFloat);

                if (isFloat) {
                    bool is32 = ty ? (ty->bitWidth == 32) : false;
                    loadFloatOperand(fn, inst->operands[0], 9, is32, slots, os); // in s9 or d9
                    if (is32) {
                        os << "    str s9, [x10]\n";
                    } else {
                        os << "    str d9, [x10]\n";
                    }
                } else if (ty && (ty->isStruct() || ty->isArray())) {
                    size_t sz = getTypeSize(ty, currentMod);
                    if (sz == 0 && ty->isStruct() && currentMod) sz = currentMod->getStructSize(ty->nominalName());
                    if (sz == 0) sz = 8;
                    loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os); // src ptr in x9
                    for (size_t b = 0; b < sz; b += 8) {
                        if (b + 8 <= sz) {
                            os << "    ldr x11, [x9, #" << b << "]\n";
                            os << "    str x11, [x10, #" << b << "]\n";
                        } else {
                            for (size_t rem = b; rem < sz; ++rem) {
                                os << "    ldrb w11, [x9, #" << rem << "]\n";
                                os << "    strb w11, [x10, #" << rem << "]\n";
                            }
                        }
                    }
                } else {
                    loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os);  // val in x9
                    if (ty && ty->isInt() && ty->bitWidth == 8) {
                        os << "    strb w9, [x10]\n";
                    } else if (ty && ty->isInt() && ty->bitWidth == 16) {
                        os << "    strh w9, [x10]\n";
                    } else if (ty && ty->isInt() && ty->bitWidth == 32) {
                        os << "    str w9, [x10]\n";
                    } else {
                        os << "    str x9, [x10]\n";
                    }
                }
                break;
            }

            case Opcode::Load: {
                loadOperand(fn, inst->operands[0], 10, slots, isAlloca, os); // ptr in x10
                TypePtr ty = inst->type;
                size_t off = slots.at(inst->resultVar);
                if (ty && ty->isFloat()) {
                    if (ty->bitWidth == 32) {
                        os << "    ldr s9, [x10]\n";
                        emitStr32("s9", "sp", off, os);
                    } else {
                        os << "    ldr d9, [x10]\n";
                        emitStr("d9", "sp", off, os);
                    }
                } else if (ty && ty->isInt() && ty->bitWidth == 8) {
                    os << (ty->isSigned ? "    ldrsb w9, [x10]\n" : "    ldrb w9, [x10]\n");
                    emitStr("x9", "sp", off, os);
                } else if (ty && ty->isInt() && ty->bitWidth == 16) {
                    os << (ty->isSigned ? "    ldrsh w9, [x10]\n" : "    ldrh w9, [x10]\n");
                    emitStr("x9", "sp", off, os);
                } else if (ty && ty->isInt() && ty->bitWidth == 32) {
                    os << "    ldr w9, [x10]\n";
                    emitStr("x9", "sp", off, os);
                } else {
                    os << "    ldr x9, [x10]\n";
                    emitStr("x9", "sp", off, os);
                }
                break;
            }

            case Opcode::FieldPtr: {
                std::string structName = inst->operands[0].text;
                loadOperand(fn, inst->operands[1], 10, slots, isAlloca, os); // base ptr in x10
                std::string fieldName = inst->operands[2].text;
                size_t off = currentMod ? currentMod->getFieldOffset(structName, fieldName) : 0;
                if (off == 0 && (structName.empty() || structName == "slice")) {
                    if (fieldName == "len") off = 8;
                    else if (fieldName == "ptr" || fieldName == "data") off = 0;
                }
                if (off > 0) {
                    emitAddImm("x9", "x10", off, os);
                } else {
                    os << "    mov x9, x10\n";
                }
                size_t resOff = slots.at(inst->resultVar);
                emitStr("x9", "sp", resOff, os);
                break;
            }

            case Opcode::OffsetPtr: {
                loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os);  // base in x9
                loadOperand(fn, inst->operands[1], 10, slots, isAlloca, os); // bytes in x10
                os << "    add x9, x9, x10\n";
                size_t resOff = slots.at(inst->resultVar);
                emitStr("x9", "sp", resOff, os);
                break;
            }

            case Opcode::Gep: {
                loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os);  // base in x9
                loadOperand(fn, inst->operands[1], 10, slots, isAlloca, os); // index in x10
                TypePtr elemType = inst->type->pointee ? inst->type->pointee : (inst->type->elemType ? inst->type->elemType : inst->type);
                size_t elemSize = getTypeSize(elemType, currentMod);
                if (elemSize == 0 && elemType && elemType->isStruct() && currentMod) {
                    elemSize = currentMod->getStructSize(elemType->nominalName());
                }
                if (elemSize == 0) elemSize = 8;

                if (elemSize == 1) {
                    os << "    add x9, x9, x10\n";
                } else if (elemSize == 2) {
                    os << "    add x9, x9, x10, lsl #1\n";
                } else if (elemSize == 4) {
                    os << "    add x9, x9, x10, lsl #2\n";
                } else if (elemSize == 8) {
                    os << "    add x9, x9, x10, lsl #3\n";
                } else {
                    emitIntImm(static_cast<int64_t>(elemSize), "x11", os);
                    os << "    mul x10, x10, x11\n";
                    os << "    add x9, x9, x10\n";
                }
                size_t resOff = slots.at(inst->resultVar);
                emitStr("x9", "sp", resOff, os);
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
                emitStr("x9", "sp", off, os);
                break;
            }

            case Opcode::SRem:
            case Opcode::URem: {
                loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os);
                loadOperand(fn, inst->operands[1], 10, slots, isAlloca, os);
                bool is32 = inst->type && inst->type->isInt() && inst->type->bitWidth <= 32;
                if (is32) {
                    if (inst->op == Opcode::SRem) os << "    sdiv w11, w9, w10\n";
                    else os << "    udiv w11, w9, w10\n";
                    os << "    msub w9, w11, w10, w9\n";
                } else {
                    if (inst->op == Opcode::SRem) os << "    sdiv x11, x9, x10\n";
                    else os << "    udiv x11, x9, x10\n";
                    os << "    msub x9, x11, x10, x9\n";
                }
                emitStr("x9", "sp", slots.at(inst->resultVar), os);
                break;
            }

            case Opcode::FAdd:
            case Opcode::FSub:
            case Opcode::FMul:
            case Opcode::FDiv: {
                bool is32 = inst->type && inst->type->bitWidth == 32;
                loadFloatOperand(fn, inst->operands[0], 9, is32, slots, os);
                loadFloatOperand(fn, inst->operands[1], 10, is32, slots, os);
                std::string rd = (is32 ? "s9" : "d9");
                std::string rn = (is32 ? "s9" : "d9");
                std::string rm = (is32 ? "s10" : "d10");
                if (inst->op == Opcode::FAdd) os << "    fadd " << rd << ", " << rn << ", " << rm << "\n";
                else if (inst->op == Opcode::FSub) os << "    fsub " << rd << ", " << rn << ", " << rm << "\n";
                else if (inst->op == Opcode::FMul) os << "    fmul " << rd << ", " << rn << ", " << rm << "\n";
                else if (inst->op == Opcode::FDiv) os << "    fdiv " << rd << ", " << rn << ", " << rm << "\n";
                size_t off = slots.at(inst->resultVar);
                if (is32) emitStr32(rd, "sp", off, os);
                else emitStr(rd, "sp", off, os);
                break;
            }

            case Opcode::And:
            case Opcode::Or:
            case Opcode::Xor:
            case Opcode::Shl:
            case Opcode::LShr:
            case Opcode::AShr: {
                loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os);
                loadOperand(fn, inst->operands[1], 10, slots, isAlloca, os);
                bool is32 = inst->type && inst->type->isInt() && inst->type->bitWidth <= 32;
                std::string rd = is32 ? "w9" : "x9";
                std::string rn = is32 ? "w9" : "x9";
                std::string rm = is32 ? "w10" : "x10";
                if (inst->op == Opcode::And) os << "    and " << rd << ", " << rn << ", " << rm << "\n";
                else if (inst->op == Opcode::Or) os << "    orr " << rd << ", " << rn << ", " << rm << "\n";
                else if (inst->op == Opcode::Xor) os << "    eor " << rd << ", " << rn << ", " << rm << "\n";
                else if (inst->op == Opcode::Shl) os << "    lsl " << rd << ", " << rn << ", " << rm << "\n";
                else if (inst->op == Opcode::LShr) os << "    lsr " << rd << ", " << rn << ", " << rm << "\n";
                else if (inst->op == Opcode::AShr) os << "    asr " << rd << ", " << rn << ", " << rm << "\n";
                size_t off = slots.at(inst->resultVar);
                emitStr("x9", "sp", off, os);
                break;
            }

            case Opcode::Not: {
                loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os);
                bool is32 = inst->type && inst->type->isInt() && inst->type->bitWidth <= 32;
                if (is32) os << "    mvn w9, w9\n";
                else os << "    mvn x9, x9\n";
                size_t off = slots.at(inst->resultVar);
                emitStr("x9", "sp", off, os);
                break;
            }

            case Opcode::Neg: {
                if (inst->type && inst->type->isFloat()) {
                    bool is32 = inst->type->bitWidth == 32;
                    loadFloatOperand(fn, inst->operands[0], 9, is32, slots, os);
                    if (is32) {
                        os << "    fneg s9, s9\n";
                        emitStr32("s9", "sp", slots.at(inst->resultVar), os);
                    } else {
                        os << "    fneg d9, d9\n";
                        emitStr("d9", "sp", slots.at(inst->resultVar), os);
                    }
                } else {
                    loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os);
                    bool is32 = inst->type && inst->type->isInt() && inst->type->bitWidth <= 32;
                    if (is32) os << "    neg w9, w9\n";
                    else os << "    neg x9, x9\n";
                    emitStr("x9", "sp", slots.at(inst->resultVar), os);
                }
                break;
            }

            case Opcode::ICmp: {
                loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os);
                loadOperand(fn, inst->operands[1], 10, slots, isAlloca, os);
                bool is32 = inst->operands[0].type && inst->operands[0].type->isInt() && inst->operands[0].type->bitWidth <= 32;
                if (is32) {
                    os << "    cmp w9, w10\n";
                } else {
                    os << "    cmp x9, x10\n";
                }
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
                emitStr("x9", "sp", off, os);
                break;
            }

            case Opcode::FCmp: {
                bool is32 = inst->operands[0].type && inst->operands[0].type->bitWidth == 32;
                loadFloatOperand(fn, inst->operands[0], 9, is32, slots, os);
                loadFloatOperand(fn, inst->operands[1], 10, is32, slots, os);
                if (is32) os << "    fcmp s9, s10\n";
                else os << "    fcmp d9, d10\n";
                std::string condStr = "eq";
                switch (inst->fcmpCond) {
                    case FCmpCond::Oeq: condStr = "eq"; break;
                    case FCmpCond::One: condStr = "ne"; break;
                    case FCmpCond::Olt: condStr = "mi"; break;
                    case FCmpCond::Ole: condStr = "ls"; break;
                    case FCmpCond::Ogt: condStr = "gt"; break;
                    case FCmpCond::Oge: condStr = "ge"; break;
                }
                os << "    cset x9, " << condStr << "\n";
                emitStr("x9", "sp", slots.at(inst->resultVar), os);
                break;
            }

            case Opcode::SIToFP: {
                loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os);
                bool is32 = inst->type && inst->type->bitWidth == 32;
                bool src32 = inst->operands[0].type && inst->operands[0].type->bitWidth <= 32;
                if (is32) {
                    os << (src32 ? "    scvtf s9, w9\n" : "    scvtf s9, x9\n");
                    emitStr32("s9", "sp", slots.at(inst->resultVar), os);
                } else {
                    os << (src32 ? "    scvtf d9, w9\n" : "    scvtf d9, x9\n");
                    emitStr("d9", "sp", slots.at(inst->resultVar), os);
                }
                break;
            }

            case Opcode::FPToSI: {
                bool src32 = inst->operands[0].type && inst->operands[0].type->bitWidth == 32;
                loadFloatOperand(fn, inst->operands[0], 9, src32, slots, os);
                bool dst32 = inst->type && inst->type->bitWidth <= 32;
                if (src32) {
                    os << (dst32 ? "    fcvtzs w9, s9\n" : "    fcvtzs x9, s9\n");
                } else {
                    os << (dst32 ? "    fcvtzs w9, d9\n" : "    fcvtzs x9, d9\n");
                }
                emitStr("x9", "sp", slots.at(inst->resultVar), os);
                break;
            }

            case Opcode::FPTrunc: {
                loadFloatOperand(fn, inst->operands[0], 9, false, slots, os); // d9
                os << "    fcvt s9, d9\n";
                emitStr32("s9", "sp", slots.at(inst->resultVar), os);
                break;
            }

            case Opcode::FPExt: {
                loadFloatOperand(fn, inst->operands[0], 9, true, slots, os); // s9
                os << "    fcvt d9, s9\n";
                emitStr("d9", "sp", slots.at(inst->resultVar), os);
                break;
            }

            case Opcode::Trunc: {
                loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os);
                if (inst->type && inst->type->bitWidth == 1) {
                    os << "    and x9, x9, #1\n";
                } else if (inst->type && inst->type->bitWidth == 8) {
                    os << "    uxtb w9, w9\n";
                } else if (inst->type && inst->type->bitWidth == 16) {
                    os << "    uxth w9, w9\n";
                }
                emitStr("x9", "sp", slots.at(inst->resultVar), os);
                break;
            }

            case Opcode::SExt: {
                loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os);
                size_t bw = inst->operands[0].type ? inst->operands[0].type->bitWidth : 32;
                if (bw == 1) os << "    sbfx x9, x9, #0, #1\n";
                else if (bw == 8) os << "    sxtb x9, w9\n";
                else if (bw == 16) os << "    sxth x9, w9\n";
                else if (bw == 32) os << "    sxtw x9, w9\n";
                emitStr("x9", "sp", slots.at(inst->resultVar), os);
                break;
            }

            case Opcode::ZExt: {
                loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os);
                size_t bw = inst->operands[0].type ? inst->operands[0].type->bitWidth : 32;
                if (bw == 1) os << "    and x9, x9, #1\n";
                else if (bw == 8) os << "    uxtb w9, w9\n";
                else if (bw == 16) os << "    uxth w9, w9\n";
                else if (bw == 32) os << "    mov w9, w9\n";
                emitStr("x9", "sp", slots.at(inst->resultVar), os);
                break;
            }

            case Opcode::Bitcast:
            case Opcode::PtrToInt:
            case Opcode::IntToPtr: {
                if (inst->operands[0].type && inst->operands[0].type->isFloat()) {
                    bool is32 = inst->operands[0].type->bitWidth == 32;
                    loadFloatOperand(fn, inst->operands[0], 9, is32, slots, os);
                    if (is32) os << "    fmov w9, s9\n";
                    else os << "    fmov x9, d9\n";
                } else if (inst->type && inst->type->isFloat()) {
                    loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os);
                    bool is32 = inst->type->bitWidth == 32;
                    if (is32) os << "    fmov s9, w9\n";
                    else os << "    fmov d9, x9\n";
                    if (is32) emitStr32("s9", "sp", slots.at(inst->resultVar), os);
                    else emitStr("d9", "sp", slots.at(inst->resultVar), os);
                    break;
                } else {
                    loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os);
                }
                emitStr("x9", "sp", slots.at(inst->resultVar), os);
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

            case Opcode::Switch: {
                loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os); // subject in x9
                for (const auto& sc : inst->switchCases) {
                    emitIntImm(sc.first, "x10", os);
                    os << "    cmp x9, x10\n";
                    std::string target = sc.second;
                    if (!target.empty() && target[0] == '$') target = target.substr(1);
                    os << "    b.eq .L_" << fn->name << "_" << target << "\n";
                }
                if (!inst->defaultLabel.empty()) {
                    std::string defBB = inst->defaultLabel;
                    if (!defBB.empty() && defBB[0] == '$') defBB = defBB.substr(1);
                    os << "    b .L_" << fn->name << "_" << defBB << "\n";
                }
                break;
            }

            case Opcode::Call: {
                std::string targetName = inst->operands[0].text;
                if (!targetName.empty() && targetName[0] == '@') targetName = targetName.substr(1);

                const Function* targetFn = currentMod ? currentMod->getFunction(targetName) : nullptr;
                bool isVar = targetFn ? targetFn->isVariadic : (targetName.find("printf") != std::string::npos);
                size_t fixedCount = targetFn ? targetFn->params.size() : (targetName.find("printf") != std::string::npos ? 1 : 0);

                if (isDarwin && isVar) {
                    int intReg = 0;
                    int fpReg = 0;
                    size_t varStackIdx = 0;

                    for (size_t i = 1; i < inst->operands.size(); ++i) {
                        const auto& op = inst->operands[i];
                        bool opIsFloat = (op.type && op.type->isFloat()) || (op.kind == OperandKind::ConstantFloat);

                        if (i <= fixedCount) {
                            if (opIsFloat) {
                                if (fpReg < 8) {
                                    bool is32 = op.type && op.type->bitWidth == 32;
                                    loadFloatOperand(fn, op, fpReg++, is32, slots, os);
                                }
                            } else {
                                if (intReg < 8) {
                                    loadOperand(fn, op, intReg++, slots, isAlloca, os);
                                }
                            }
                        } else {
                            size_t stackOffset = varStackIdx * 8;
                            varStackIdx++;
                            if (opIsFloat) {
                                loadFloatOperand(fn, op, 9, false, slots, os); // in d9
                                emitStr("d9", "sp", stackOffset, os);
                            } else {
                                loadOperand(fn, op, 9, slots, isAlloca, os); // in x9
                                emitStr("x9", "sp", stackOffset, os);
                            }
                        }
                    }
                } else {
                    int intReg = 0;
                    int fpReg = 0;
                    size_t stackIdx = 0;

                    for (size_t i = 1; i < inst->operands.size(); ++i) {
                        const auto& op = inst->operands[i];
                        bool opIsFloat = (op.type && op.type->isFloat()) || (op.kind == OperandKind::ConstantFloat);

                        if (opIsFloat) {
                            if (fpReg < 8) {
                                bool is32 = op.type && op.type->bitWidth == 32;
                                loadFloatOperand(fn, op, fpReg++, is32, slots, os);
                            } else {
                                bool is32 = op.type && op.type->bitWidth == 32;
                                loadFloatOperand(fn, op, 9, is32, slots, os);
                                emitStr(is32 ? "s9" : "d9", "sp", stackIdx * 8, os);
                                stackIdx++;
                            }
                        } else {
                            if (intReg < 8) {
                                loadOperand(fn, op, intReg++, slots, isAlloca, os);
                            } else {
                                loadOperand(fn, op, 9, slots, isAlloca, os);
                                emitStr("x9", "sp", stackIdx * 8, os);
                                stackIdx++;
                            }
                        }
                    }
                }

                os << "    bl " << mangleSym(targetName) << "\n";

                if (!inst->resultVar.empty()) {
                    size_t off = slots.at(inst->resultVar);
                    if (inst->type && inst->type->isFloat()) {
                        if (inst->type->bitWidth == 32) {
                            emitStr32("s0", "sp", off, os);
                        } else {
                            emitStr("d0", "sp", off, os);
                        }
                    } else {
                        emitStr("x0", "sp", off, os);
                    }
                }
                break;
            }

            case Opcode::CallIndirect: {
                loadOperand(fn, inst->operands[0], 8, slots, isAlloca, os); // fn ptr or closure in x8
                if (inst->operands[0].type && (inst->operands[0].type->isClosure() || inst->operands[0].type->isInterface())) {
                    os << "    ldr x8, [x8]\n";
                }

                int intReg = 0;
                int fpReg = 0;
                size_t stackIdx = 0;

                for (size_t i = 1; i < inst->operands.size(); ++i) {
                    const auto& op = inst->operands[i];
                    bool opIsFloat = (op.type && op.type->isFloat()) || (op.kind == OperandKind::ConstantFloat);

                    if (opIsFloat) {
                        if (fpReg < 8) {
                            bool is32 = op.type && op.type->bitWidth == 32;
                            loadFloatOperand(fn, op, fpReg++, is32, slots, os);
                        } else {
                            bool is32 = op.type && op.type->bitWidth == 32;
                            loadFloatOperand(fn, op, 9, is32, slots, os);
                            emitStr(is32 ? "s9" : "d9", "sp", stackIdx * 8, os);
                            stackIdx++;
                        }
                    } else {
                        if (intReg < 8) {
                            loadOperand(fn, op, intReg++, slots, isAlloca, os);
                        } else {
                            loadOperand(fn, op, 9, slots, isAlloca, os);
                            emitStr("x9", "sp", stackIdx * 8, os);
                            stackIdx++;
                        }
                    }
                }

                os << "    blr x8\n";

                if (!inst->resultVar.empty()) {
                    size_t off = slots.at(inst->resultVar);
                    if (inst->type && inst->type->isFloat()) {
                        if (inst->type->bitWidth == 32) {
                            emitStr32("s0", "sp", off, os);
                        } else {
                            emitStr("d0", "sp", off, os);
                        }
                    } else {
                        emitStr("x0", "sp", off, os);
                    }
                }
                break;
            }

            case Opcode::MakeClosure:
            case Opcode::MakeInterface:
            case Opcode::MakeSlice: {
                size_t slotOff = slots.at(inst->resultVar);
                loadOperand(fn, inst->operands[0], 9, slots, isAlloca, os);
                emitStr("x9", "sp", slotOff, os);
                loadOperand(fn, inst->operands[1], 9, slots, isAlloca, os);
                emitStr("x9", "sp", slotOff + 8, os);
                break;
            }

            case Opcode::ExtractFat: {
                loadOperand(fn, inst->operands[0], 10, slots, isAlloca, os); // fat ptr in x10
                int64_t idx = inst->operands[1].intVal;
                size_t off = (idx == 0) ? 0 : 8;
                if (off > 0) {
                    emitLdr("x9", "x10", off, os);
                } else {
                    os << "    ldr x9, [x10]\n";
                }
                size_t resOff = slots.at(inst->resultVar);
                emitStr("x9", "sp", resOff, os);
                break;
            }

            case Opcode::Ret: {
                if (!inst->operands.empty()) {
                    const auto& op = inst->operands[0];
                    bool opIsFloat = (inst->type && inst->type->isFloat()) ||
                                     (op.type && op.type->isFloat()) ||
                                     (op.kind == OperandKind::ConstantFloat);
                    if (opIsFloat) {
                        bool is32 = inst->type ? (inst->type->bitWidth == 32) : false;
                        loadFloatOperand(fn, op, 0, is32, slots, os);
                    } else {
                        loadOperand(fn, op, 0, slots, isAlloca, os);
                    }
                }
                os << "    b " << retLabel << "\n";
                break;
            }

            case Opcode::InlineAsm: {
                if (!inst->asmString.empty()) {
                    os << "    " << inst->asmString << "\n";
                }
                break;
            }

            case Opcode::Unreachable: {
                os << "    brk #1\n";
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
