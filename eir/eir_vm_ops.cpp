// eir_vm_ops.cpp: the interpreter's memory, arithmetic, comparison and conversion instructions (split from eir_vm_exec.cpp).
#include "eir_vm.h"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include "llvm/Support/DynamicLibrary.h"
#include <thread>
#include <mutex>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif


namespace eir {

// An integer `/` or `%` by zero stops the program, as the compiled one traps.
[[noreturn]] static void divisionByZero() {
    std::cerr << "error: integer division by zero" << std::endl;
    std::abort();
}

void VM::execMemory(const std::unique_ptr<Instruction>& inst, StackFrame& frame) {
    switch (inst->op) {
        case Opcode::Alloca: {
            size_t sz = 0;
            if (inst->type->isStruct() && currentModule) {
                sz = currentModule->getStructSize(inst->type->nominalName());
            }
            if (sz == 0) sz = getTypeSize(inst->type, currentModule);
            if (sz == 0) sz = 8;
            size_t al = inst->operands.empty() ? 8 : static_cast<size_t>(inst->operands[0].intVal);
            if (inst->type->isStruct() && al < 8) al = 8;
            void* slot = frame.allocStack(sz, al);
            frame.registers[inst->resultVar] = VMValue(slot);
            break;
        }
        case Opcode::Load: {
            void* ptr = getOperand(inst->operands[0], frame).asPtr();
            frame.registers[inst->resultVar] = readMemory(ptr, inst->type);
            break;
        }
        case Opcode::Store: {
            VMValue val = getOperand(inst->operands[0], frame);
            void* ptr = getOperand(inst->operands[1], frame).asPtr();
            TypePtr destType = (inst->operands[1].type && inst->operands[1].type->pointee)
                               ? inst->operands[1].type->pointee
                               : inst->operands[0].type;
            writeMemory(ptr, destType, val);
            break;
        }
        case Opcode::Gep: {
            void* base = getOperand(inst->operands[0], frame).asPtr();
            int64_t idx = getOperand(inst->operands[1], frame).asInt();
            TypePtr elemType = inst->type->pointee ? inst->type->pointee : (inst->type->elemType ? inst->type->elemType : inst->type);
            size_t elemSize = getTypeSize(elemType, currentModule);
            if (elemSize == 0 && elemType->isStruct() && currentModule) {
                elemSize = currentModule->getStructSize(elemType->nominalName());
            }
            if (elemSize == 0) elemSize = 8;

            void* res = static_cast<uint8_t*>(base) + idx * elemSize;
            frame.registers[inst->resultVar] = VMValue(res);
            break;
        }
        case Opcode::FieldPtr: {
            std::string structName = inst->operands[0].text;
            void* base = getOperand(inst->operands[1], frame).asPtr();
            std::string fieldName = inst->operands[2].text;
            size_t off = currentModule ? currentModule->getFieldOffset(structName, fieldName) : 0;
            if (off == 0 && (structName.empty() || structName == "slice")) {
                if (fieldName == "len") off = 8;
                else if (fieldName == "ptr" || fieldName == "data") off = 0;
            }
            void* res = static_cast<uint8_t*>(base) + off;
            frame.registers[inst->resultVar] = VMValue(res);
            break;
        }
        case Opcode::OffsetPtr: {
            void* base = getOperand(inst->operands[0], frame).asPtr();
            int64_t bytes = getOperand(inst->operands[1], frame).asInt();
            void* res = static_cast<uint8_t*>(base) + bytes;
            frame.registers[inst->resultVar] = VMValue(res);
            break;
        }
        default:
            break;
    }
}

void VM::execIntArith(const std::unique_ptr<Instruction>& inst, StackFrame& frame) {
    switch (inst->op) {
        case Opcode::Add: {
            int64_t a = getOperand(inst->operands[0], frame).asInt();
            int64_t b = getOperand(inst->operands[1], frame).asInt();
            if (inst->type && inst->type->bitWidth == 32) {
                int32_t res = static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
                frame.registers[inst->resultVar] = VMValue(inst->type->isSigned ? static_cast<int64_t>(res) : static_cast<int64_t>(static_cast<uint32_t>(res)));
            } else {
                uint64_t res = static_cast<uint64_t>(a) + static_cast<uint64_t>(b);
                frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(res));
            }
            break;
        }
        case Opcode::Sub: {
            int64_t a = getOperand(inst->operands[0], frame).asInt();
            int64_t b = getOperand(inst->operands[1], frame).asInt();
            if (inst->type && inst->type->bitWidth == 32) {
                int32_t res = static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
                frame.registers[inst->resultVar] = VMValue(inst->type->isSigned ? static_cast<int64_t>(res) : static_cast<int64_t>(static_cast<uint32_t>(res)));
            } else {
                uint64_t res = static_cast<uint64_t>(a) - static_cast<uint64_t>(b);
                frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(res));
            }
            break;
        }
        case Opcode::Mul: {
            int64_t a = getOperand(inst->operands[0], frame).asInt();
            int64_t b = getOperand(inst->operands[1], frame).asInt();
            if (inst->type && inst->type->bitWidth == 32) {
                int32_t res = static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
                frame.registers[inst->resultVar] = VMValue(inst->type->isSigned ? static_cast<int64_t>(res) : static_cast<int64_t>(static_cast<uint32_t>(res)));
            } else {
                uint64_t res = static_cast<uint64_t>(a) * static_cast<uint64_t>(b);
                frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(res));
            }
            break;
        }
        case Opcode::SDiv: {
            int64_t a = getOperand(inst->operands[0], frame).asInt();
            int64_t b = getOperand(inst->operands[1], frame).asInt();
            if (b == 0) {
                divisionByZero();
            } else if (inst->type && inst->type->bitWidth == 32) {
                int32_t a32 = static_cast<int32_t>(a);
                int32_t b32 = static_cast<int32_t>(b);
                if (b32 == -1 && a32 == -2147483648LL) {
                    frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(a32));
                } else if (b32 == 0) {
                    frame.registers[inst->resultVar] = VMValue(0);
                } else {
                    frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(a32 / b32));
                }
            } else {
                if (b == -1 && a == std::numeric_limits<int64_t>::min()) {
                    frame.registers[inst->resultVar] = VMValue(a);
                } else {
                    frame.registers[inst->resultVar] = VMValue(a / b);
                }
            }
            break;
        }
        case Opcode::UDiv: {
            uint64_t a = static_cast<uint64_t>(getOperand(inst->operands[0], frame).asInt());
            uint64_t b = static_cast<uint64_t>(getOperand(inst->operands[1], frame).asInt());
            if (b == 0) divisionByZero();
            frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(a / b));
            break;
        }
        case Opcode::SRem: {
            int64_t a = getOperand(inst->operands[0], frame).asInt();
            int64_t b = getOperand(inst->operands[1], frame).asInt();
            if (b == 0) {
                divisionByZero();
            } else if (inst->type && inst->type->bitWidth == 32) {
                int32_t a32 = static_cast<int32_t>(a);
                int32_t b32 = static_cast<int32_t>(b);
                if (b32 == -1 && a32 == -2147483648LL) {
                    frame.registers[inst->resultVar] = VMValue(0);
                } else if (b32 == 0) {
                    frame.registers[inst->resultVar] = VMValue(0);
                } else {
                    frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(a32 % b32));
                }
            } else {
                if (b == -1 && a == std::numeric_limits<int64_t>::min()) {
                    frame.registers[inst->resultVar] = VMValue(0);
                } else {
                    frame.registers[inst->resultVar] = VMValue(a % b);
                }
            }
            break;
        }
        case Opcode::URem: {
            uint64_t a = static_cast<uint64_t>(getOperand(inst->operands[0], frame).asInt());
            uint64_t b = static_cast<uint64_t>(getOperand(inst->operands[1], frame).asInt());
            if (b == 0) divisionByZero();
            frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(a % b));
            break;
        }
        default:
            break;
    }
}

void VM::execFloatArith(const std::unique_ptr<Instruction>& inst, StackFrame& frame) {
    switch (inst->op) {
        case Opcode::FAdd: {
            double a = getOperand(inst->operands[0], frame).asFloat();
            double b = getOperand(inst->operands[1], frame).asFloat();
            frame.registers[inst->resultVar] = VMValue(a + b);
            break;
        }
        case Opcode::FSub: {
            double a = getOperand(inst->operands[0], frame).asFloat();
            double b = getOperand(inst->operands[1], frame).asFloat();
            frame.registers[inst->resultVar] = VMValue(a - b);
            break;
        }
        case Opcode::FMul: {
            double a = getOperand(inst->operands[0], frame).asFloat();
            double b = getOperand(inst->operands[1], frame).asFloat();
            frame.registers[inst->resultVar] = VMValue(a * b);
            break;
        }
        case Opcode::FDiv: {
            double a = getOperand(inst->operands[0], frame).asFloat();
            double b = getOperand(inst->operands[1], frame).asFloat();
            frame.registers[inst->resultVar] = VMValue(a / b);
            break;
        }
        case Opcode::Neg: {
            int64_t a = getOperand(inst->operands[0], frame).asInt();
            frame.registers[inst->resultVar] = VMValue(-a);
            break;
        }
        default:
            break;
    }
}

void VM::execBitwise(const std::unique_ptr<Instruction>& inst, StackFrame& frame) {
    switch (inst->op) {
        case Opcode::And: {
            int64_t a = getOperand(inst->operands[0], frame).asInt();
            int64_t b = getOperand(inst->operands[1], frame).asInt();
            int64_t res = a & b;
            if (inst->type && !inst->type->isSigned) {
                if (inst->type->bitWidth == 8) res = static_cast<uint8_t>(res);
                else if (inst->type->bitWidth == 16) res = static_cast<uint16_t>(res);
                else if (inst->type->bitWidth == 32) res = static_cast<uint32_t>(res);
            }
            frame.registers[inst->resultVar] = VMValue(res);
            break;
        }
        case Opcode::Or: {
            int64_t a = getOperand(inst->operands[0], frame).asInt();
            int64_t b = getOperand(inst->operands[1], frame).asInt();
            int64_t res = a | b;
            if (inst->type && !inst->type->isSigned) {
                if (inst->type->bitWidth == 8) res = static_cast<uint8_t>(res);
                else if (inst->type->bitWidth == 16) res = static_cast<uint16_t>(res);
                else if (inst->type->bitWidth == 32) res = static_cast<uint32_t>(res);
            }
            frame.registers[inst->resultVar] = VMValue(res);
            break;
        }
        case Opcode::Xor: {
            int64_t a = getOperand(inst->operands[0], frame).asInt();
            int64_t b = getOperand(inst->operands[1], frame).asInt();
            int64_t res = a ^ b;
            if (inst->type && !inst->type->isSigned) {
                if (inst->type->bitWidth == 8) res = static_cast<uint8_t>(res);
                else if (inst->type->bitWidth == 16) res = static_cast<uint16_t>(res);
                else if (inst->type->bitWidth == 32) res = static_cast<uint32_t>(res);
            }
            frame.registers[inst->resultVar] = VMValue(res);
            break;
        }
        case Opcode::Shl: {
            int64_t a = getOperand(inst->operands[0], frame).asInt();
            int64_t b = getOperand(inst->operands[1], frame).asInt();
            if (inst->type && inst->type->bitWidth == 8) {
                b &= 7;
                uint8_t ua = static_cast<uint8_t>(a);
                frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(static_cast<uint8_t>(ua << b)));
            } else if (inst->type && inst->type->bitWidth == 16) {
                b &= 15;
                uint16_t ua = static_cast<uint16_t>(a);
                frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(static_cast<uint16_t>(ua << b)));
            } else if (inst->type && inst->type->bitWidth == 32) {
                b &= 31;
                uint32_t ua = static_cast<uint32_t>(a);
                uint32_t res = ua << b;
                frame.registers[inst->resultVar] = VMValue(inst->type->isSigned
                    ? static_cast<int64_t>(static_cast<int32_t>(res))
                    : static_cast<int64_t>(res));
            } else {
                b &= 63;
                uint64_t ua = static_cast<uint64_t>(a);
                uint64_t res = ua << b;
                frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(res));
            }
            break;
        }
        case Opcode::LShr: {
            uint64_t a = static_cast<uint64_t>(getOperand(inst->operands[0], frame).asInt());
            uint64_t b = static_cast<uint64_t>(getOperand(inst->operands[1], frame).asInt());
            if (inst->type && inst->type->bitWidth == 8) {
                b &= 7;
                uint8_t ua = static_cast<uint8_t>(a);
                frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(ua >> b));
            } else if (inst->type && inst->type->bitWidth == 16) {
                b &= 15;
                uint16_t ua = static_cast<uint16_t>(a);
                frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(ua >> b));
            } else if (inst->type && inst->type->bitWidth == 32) {
                b &= 31;
                uint32_t ua = static_cast<uint32_t>(a);
                frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(ua >> b));
            } else {
                b &= 63;
                frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(a >> b));
            }
            break;
        }
        case Opcode::AShr: {
            int64_t a = getOperand(inst->operands[0], frame).asInt();
            int64_t b = getOperand(inst->operands[1], frame).asInt();
            if (inst->type && inst->type->bitWidth == 8) {
                b &= 7;
                int8_t sa = static_cast<int8_t>(a);
                frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(sa >> b));
            } else if (inst->type && inst->type->bitWidth == 16) {
                b &= 15;
                int16_t sa = static_cast<int16_t>(a);
                frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(sa >> b));
            } else if (inst->type && inst->type->bitWidth == 32) {
                b &= 31;
                int32_t sa = static_cast<int32_t>(a);
                frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(sa >> b));
            } else {
                b &= 63;
                frame.registers[inst->resultVar] = VMValue(a >> b);
            }
            break;
        }
        case Opcode::Not: {
            int64_t a = getOperand(inst->operands[0], frame).asInt();
            frame.registers[inst->resultVar] = VMValue(~a);
            break;
        }
        default:
            break;
    }
}

void VM::execCompare(const std::unique_ptr<Instruction>& inst, StackFrame& frame) {
    switch (inst->op) {
        case Opcode::ICmp: {
            int64_t a = 0;
            if (inst->operands[0].type && inst->operands[0].type->isInterface()) {
                void* slot = getOperand(inst->operands[0], frame).asPtr();
                void* dataPtr = slot ? *reinterpret_cast<void**>(static_cast<uint8_t*>(slot) + 0) : nullptr;
                a = reinterpret_cast<int64_t>(dataPtr);
            } else {
                a = getOperand(inst->operands[0], frame).asInt();
            }

            int64_t b = 0;
            if (inst->operands[1].type && inst->operands[1].type->isInterface()) {
                void* slot = getOperand(inst->operands[1], frame).asPtr();
                void* dataPtr = slot ? *reinterpret_cast<void**>(static_cast<uint8_t*>(slot) + 0) : nullptr;
                b = reinterpret_cast<int64_t>(dataPtr);
            } else {
                b = getOperand(inst->operands[1], frame).asInt();
            }
            uint64_t ua = static_cast<uint64_t>(a);
            uint64_t ub = static_cast<uint64_t>(b);
            bool res = false;
            switch (inst->icmpCond) {
                case ICmpCond::Eq:  res = (a == b); break;
                case ICmpCond::Ne:  res = (a != b); break;
                case ICmpCond::Slt: res = (a < b); break;
                case ICmpCond::Sle: res = (a <= b); break;
                case ICmpCond::Sgt: res = (a > b); break;
                case ICmpCond::Sge: res = (a >= b); break;
                case ICmpCond::Ult: res = (ua < ub); break;
                case ICmpCond::Ule: res = (ua <= ub); break;
                case ICmpCond::Ugt: res = (ua > ub); break;
                case ICmpCond::Uge: res = (ua >= ub); break;
            }
            frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(res ? 1 : 0));
            break;
        }
        case Opcode::FCmp: {
            double a = getOperand(inst->operands[0], frame).asFloat();
            double b = getOperand(inst->operands[1], frame).asFloat();
            bool res = false;
            switch (inst->fcmpCond) {
                case FCmpCond::Oeq: res = (a == b); break;
                case FCmpCond::One: res = (a != b); break;
                case FCmpCond::Olt: res = (a < b); break;
                case FCmpCond::Ole: res = (a <= b); break;
                case FCmpCond::Ogt: res = (a > b); break;
                case FCmpCond::Oge: res = (a >= b); break;
            }
            frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(res ? 1 : 0));
            break;
        }
        default:
            break;
    }
}

void VM::execConvert(const std::unique_ptr<Instruction>& inst, StackFrame& frame) {
    switch (inst->op) {
        case Opcode::Trunc: {
            int64_t a = getOperand(inst->operands[0], frame).asInt();
            if (inst->type && inst->type->bitWidth == 1) {
                a = a & 1;
            } else if (inst->type && inst->type->bitWidth == 8) {
                a = inst->type->isSigned ? static_cast<int64_t>(static_cast<int8_t>(a)) : static_cast<int64_t>(static_cast<uint8_t>(a));
            } else if (inst->type && inst->type->bitWidth == 16) {
                a = inst->type->isSigned ? static_cast<int64_t>(static_cast<int16_t>(a)) : static_cast<int64_t>(static_cast<uint16_t>(a));
            } else if (inst->type && inst->type->bitWidth == 32) {
                a = inst->type->isSigned ? static_cast<int64_t>(static_cast<int32_t>(a)) : static_cast<int64_t>(static_cast<uint32_t>(a));
            }
            frame.registers[inst->resultVar] = VMValue(a);
            break;
        }
        case Opcode::ZExt: {
            uint64_t a = static_cast<uint64_t>(getOperand(inst->operands[0], frame).asInt());
            int srcBw = inst->operands[0].type ? inst->operands[0].type->bitWidth : 32;
            if (srcBw == 1) a &= 1;
            else if (srcBw == 8) a = static_cast<uint8_t>(a);
            else if (srcBw == 16) a = static_cast<uint16_t>(a);
            else if (srcBw == 32) a = static_cast<uint32_t>(a);
            frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(a));
            break;
        }
        case Opcode::SExt: {
            int64_t a = getOperand(inst->operands[0], frame).asInt();
            int srcBw = inst->operands[0].type ? inst->operands[0].type->bitWidth : 32;
            if (srcBw == 1) a = (a & 1) ? -1 : 0;
            else if (srcBw == 8) a = static_cast<int8_t>(a);
            else if (srcBw == 16) a = static_cast<int16_t>(a);
            else if (srcBw == 32) a = static_cast<int32_t>(a);
            frame.registers[inst->resultVar] = VMValue(a);
            break;
        }
        case Opcode::Bitcast: {
            VMValue a = getOperand(inst->operands[0], frame);
            frame.registers[inst->resultVar] = a;
            break;
        }
        case Opcode::PtrToInt: {
            frame.registers[inst->resultVar] = VMValue(getOperand(inst->operands[0], frame).asInt());
            break;
        }
        case Opcode::IntToPtr: {
            frame.registers[inst->resultVar] = VMValue(getOperand(inst->operands[0], frame).asPtr());
            break;
        }
        case Opcode::FPTrunc: {
            float f = static_cast<float>(getOperand(inst->operands[0], frame).asFloat());
            frame.registers[inst->resultVar] = VMValue(static_cast<double>(f));
            break;
        }
        case Opcode::FPExt: {
            frame.registers[inst->resultVar] = VMValue(getOperand(inst->operands[0], frame).asFloat());
            break;
        }
        case Opcode::FPToSI: {
            double a = getOperand(inst->operands[0], frame).asFloat();
            int64_t res = 0;
            if (std::isnan(a)) {
                res = 0;
            } else if (inst->type) {
                bool uns = !inst->type->isSigned;
                int bw = inst->type->bitWidth;
                if (uns) {
                    if (bw == 8) {
                        if (a <= 0.0) res = 0;
                        else if (a >= 255.0) res = 255;
                        else res = static_cast<uint8_t>(a);
                    } else if (bw == 16) {
                        if (a <= 0.0) res = 0;
                        else if (a >= 65535.0) res = 65535;
                        else res = static_cast<uint16_t>(a);
                    } else if (bw == 32) {
                        if (a <= 0.0) res = 0;
                        else if (a >= 4294967295.0) res = 4294967295ULL;
                        else res = static_cast<uint32_t>(a);
                    } else {
                        if (a <= 0.0) res = 0;
                        else if (a >= 18446744073709551615.0) res = -1LL;
                        else res = static_cast<int64_t>(static_cast<uint64_t>(a));
                    }
                } else {
                    if (bw == 8) {
                        if (a <= -128.0) res = -128;
                        else if (a >= 127.0) res = 127;
                        else res = static_cast<int8_t>(a);
                    } else if (bw == 16) {
                        if (a <= -32768.0) res = -32768;
                        else if (a >= 32767.0) res = 32767;
                        else res = static_cast<int16_t>(a);
                    } else if (bw == 32) {
                        if (a <= -2147483648.0) res = -2147483648LL;
                        else if (a >= 2147483647.0) res = 2147483647LL;
                        else res = static_cast<int32_t>(a);
                    } else {
                        if (a <= -9223372036854775808.0) res = std::numeric_limits<int64_t>::min();
                        else if (a >= 9223372036854775807.0) res = std::numeric_limits<int64_t>::max();
                        else res = static_cast<int64_t>(a);
                    }
                }
            } else {
                res = static_cast<int64_t>(a);
            }
            frame.registers[inst->resultVar] = VMValue(res);
            break;
        }
        case Opcode::SIToFP: {
            int64_t a = getOperand(inst->operands[0], frame).asInt();
            frame.registers[inst->resultVar] = VMValue(static_cast<double>(a));
            break;
        }
        default:
            break;
    }
}

} // namespace eir
