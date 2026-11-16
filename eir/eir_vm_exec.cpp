// eir_vm_exec.cpp: the interpreter loop (executeFunction) and run (split from eir_vm.cpp).
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

VMValue VM::executeFunction(Function* fn, const std::vector<VMValue>& args) {
    if (!fn) return VMValue(0);
    if (fn->isExtern) {
        return callExtern(fn->name, args, fn->returnType);
    }

    StackFrame frame;
    frame.function = fn;
    size_t savedCaughtDepth = caughtExceptionStack.size();

    if (fn->isVariadic && args.size() > fn->params.size()) {
        frame.varargs.assign(args.begin() + fn->params.size(), args.end());
    }

    // Bind parameters
    for (size_t i = 0; i < fn->params.size(); ++i) {
        if (i < args.size()) {
            frame.registers["%" + fn->params[i].first] = args[i];
        }
    }

    BasicBlock* curBB = fn->blocks.empty() ? nullptr : fn->blocks[0].get();

    while (curBB) {
        BasicBlock* nextBB = nullptr;

        for (const auto& inst : curBB->instructions) {
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
                case Opcode::Br: {
                    std::string targetLabel = inst->operands[0].text;
                    for (const auto& b : fn->blocks) {
                        if (b->name == targetLabel) {
                            nextBB = b.get();
                            break;
                        }
                    }
                    break;
                }
                case Opcode::BrCond: {
                    int64_t cond = 0;
                    if (inst->operands[0].type && inst->operands[0].type->isInterface()) {
                        void* slot = getOperand(inst->operands[0], frame).asPtr();
                        void* dataPtr = slot ? *reinterpret_cast<void**>(static_cast<uint8_t*>(slot) + 0) : nullptr;
                        cond = reinterpret_cast<int64_t>(dataPtr);
                    } else {
                        cond = getOperand(inst->operands[0], frame).asInt();
                    }
                    std::string targetLabel = (cond != 0) ? inst->operands[1].text : inst->operands[2].text;
                    for (const auto& b : fn->blocks) {
                        if (b->name == targetLabel) {
                            nextBB = b.get();
                            break;
                        }
                    }
                    break;
                }
                case Opcode::Switch: {
                    int64_t val = getOperand(inst->operands[0], frame).asInt();
                    std::string targetLabel = inst->defaultLabel;
                    for (const auto& sc : inst->switchCases) {
                        if (sc.first == val) {
                            targetLabel = sc.second;
                            break;
                        }
                    }
                    for (const auto& b : fn->blocks) {
                        if (b->name == targetLabel) {
                            nextBB = b.get();
                            break;
                        }
                    }
                    break;
                }
                case Opcode::Ret: {
                    if (caughtExceptionStack.size() > savedCaughtDepth) caughtExceptionStack.resize(savedCaughtDepth);
                    if (inst->operands.empty() || inst->operands[0].type->isVoid()) {
                        return VMValue(0);
                    }
                    VMValue retVal = getOperand(inst->operands[0], frame);
                    TypePtr retTy = inst->operands[0].type;
                    if (retTy && (retTy->isStruct() || retTy->isArray() || retTy->isClosure() || retTy->isInterface() || retTy->isSlice())) {
                        size_t sz = 16;
                        if (retTy->isStruct()) {
                            sz = currentModule ? currentModule->getStructSize(retTy->nominalName()) : 0;
                            if (sz == 0) sz = getTypeSize(retTy, currentModule);
                        } else if (retTy->isArray()) {
                            sz = getTypeSize(retTy, currentModule);
                        }
                        if (sz == 0) sz = 16;
                        thread_local static std::vector<uint8_t> threadReturnBuffer;
                        threadReturnBuffer.resize(sz);
                        if (retVal.asPtr()) {
                            std::memcpy(threadReturnBuffer.data(), retVal.asPtr(), sz);
                        } else {
                            std::memset(threadReturnBuffer.data(), 0, sz);
                        }
                        return VMValue(threadReturnBuffer.data());
                    }
                    return retVal;
                }
                case Opcode::Unreachable:
                    if (caughtExceptionStack.size() > savedCaughtDepth) caughtExceptionStack.resize(savedCaughtDepth);
                    return VMValue(0);

                case Opcode::Throw: {
                    void* exPtr = getOperand(inst->operands[0], frame).asPtr();
                    currentException = exPtr;
                    if (!frame.exceptionHandlers.empty()) {
                        nextBB = frame.exceptionHandlers.back();
                        frame.exceptionHandlers.pop_back();
                    } else {
                        if (caughtExceptionStack.size() > savedCaughtDepth) caughtExceptionStack.resize(savedCaughtDepth);
                        return VMValue(0);
                    }
                    break;
                }
                case Opcode::PushHandler: {
                    std::string targetLabel = inst->operands[0].text;
                    BasicBlock* targetBB = nullptr;
                    for (const auto& b : frame.function->blocks) {
                        if (b->name == targetLabel) {
                            targetBB = b.get();
                            break;
                        }
                    }
                    if (targetBB) {
                        frame.exceptionHandlers.push_back(targetBB);
                    }
                    break;
                }
                case Opcode::PopHandler: {
                    if (!frame.exceptionHandlers.empty()) {
                        frame.exceptionHandlers.pop_back();
                    }
                    break;
                }
                case Opcode::LandingPad: {
                    if (!inst->resultVar.empty()) {
                        frame.registers[inst->resultVar] = VMValue(currentException);
                    }
                    currentException = nullptr;
                    break;
                }

                case Opcode::MakeClosure:
                case Opcode::MakeInterface:
                case Opcode::MakeSlice: {
                    void* p0 = getOperand(inst->operands[0], frame).asPtr();
                    void* p1 = inst->operands[1].type && inst->operands[1].type->isPointer()
                               ? getOperand(inst->operands[1], frame).asPtr()
                               : reinterpret_cast<void*>(static_cast<uintptr_t>(getOperand(inst->operands[1], frame).asInt()));
                    void* slot = frame.allocStack(16, 8);
                    *reinterpret_cast<void**>(static_cast<uint8_t*>(slot) + 0) = p0;
                    *reinterpret_cast<void**>(static_cast<uint8_t*>(slot) + 8) = p1;
                    frame.registers[inst->resultVar] = VMValue(slot);
                    break;
                }
                case Opcode::ExtractFat: {
                    void* fat = getOperand(inst->operands[0], frame).asPtr();
                    int64_t idx = getOperand(inst->operands[1], frame).asInt();
                    if (fat) {
                        if (idx == 0) {
                            void* p0 = *reinterpret_cast<void**>(static_cast<uint8_t*>(fat) + 0);
                            frame.registers[inst->resultVar] = VMValue(p0);
                        } else {
                            if (inst->type && inst->type->isPointer()) {
                                void* p1 = *reinterpret_cast<void**>(static_cast<uint8_t*>(fat) + 8);
                                frame.registers[inst->resultVar] = VMValue(p1);
                            } else {
                                int64_t len = *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(fat) + 8);
                                frame.registers[inst->resultVar] = VMValue(len);
                            }
                        }
                    } else {
                        frame.registers[inst->resultVar] = VMValue(0);
                    }
                    break;
                }
                case Opcode::CallIndirect: {
                    void* target = getOperand(inst->operands[0], frame).asPtr();
                    if (!target) {
                        std::cerr << "error: null closure / indirect call" << std::endl;
                        return VMValue(0);
                    }
                    Function* callee = nullptr;
                    std::vector<VMValue> callArgs;

                    if (currentModule) {
                        for (const auto& f : currentModule->functions) {
                            if (f.get() == static_cast<Function*>(target)) {
                                callee = f.get();
                                break;
                            }
                        }
                    }

                    if (callee) {
                        for (size_t i = 1; i < inst->operands.size(); ++i) {
                            callArgs.push_back(getOperand(inst->operands[i], frame));
                        }
                    } else {
                        void* fnPtr = *reinterpret_cast<void**>(static_cast<uint8_t*>(target) + 0);
                        void* envPtr = *reinterpret_cast<void**>(static_cast<uint8_t*>(target) + 8);
                        if (currentModule) {
                            for (const auto& f : currentModule->functions) {
                                if (f.get() == static_cast<Function*>(fnPtr)) {
                                    callee = f.get();
                                    break;
                                }
                            }
                        }
                        if (!callee) {
                            callee = static_cast<Function*>(fnPtr);
                        }
                        bool takesEnv = (callee && !callee->params.empty() && callee->params[0].first == "env");
                        if (takesEnv) {
                            callArgs.push_back(VMValue(envPtr)); // env is arg 0
                        }
                        for (size_t i = 1; i < inst->operands.size(); ++i) {
                            callArgs.push_back(getOperand(inst->operands[i], frame));
                        }
                    }

                    VMValue retVal = executeFunction(callee, callArgs);
                    if (inst->type && (inst->type->isStruct() || inst->type->isArray() || inst->type->isClosure() || inst->type->isInterface() || inst->type->isSlice())) {
                        size_t sz = 16;
                        if (inst->type->isStruct()) {
                            sz = currentModule ? currentModule->getStructSize(inst->type->nominalName()) : 0;
                            if (sz == 0) sz = getTypeSize(inst->type, currentModule);
                        } else if (inst->type->isArray()) {
                            sz = getTypeSize(inst->type, currentModule);
                        }
                        if (sz == 0) sz = 16;
                        void* callerSlot = frame.allocStack(sz, 8);
                        if (retVal.asPtr()) {
                            std::memcpy(callerSlot, retVal.asPtr(), sz);
                        }
                        retVal = VMValue(callerSlot);
                    }
                    if (!inst->resultVar.empty()) {
                        frame.registers[inst->resultVar] = retVal;
                    }
                    if (hasException()) {
                        if (!frame.exceptionHandlers.empty()) {
                            nextBB = frame.exceptionHandlers.back();
                            frame.exceptionHandlers.pop_back();
                        } else {
                            if (caughtExceptionStack.size() > savedCaughtDepth) caughtExceptionStack.resize(savedCaughtDepth);
                            return VMValue(0);
                        }
                    }
                    break;
                }

                case Opcode::Call: {
                    std::string targetFnName = inst->operands[0].text;
                    if (targetFnName == "va_start" || targetFnName == "llvm.va_start.p0") {
                        if (inst->operands.size() > 1) {
                            void* apPtr = getOperand(inst->operands[1], frame).asPtr();
                            if (apPtr) {
                                activeVaLists[apPtr] = VMVaList{ frame.varargs, 0 };
                                *reinterpret_cast<void**>(apPtr) = apPtr;
                            }
                        }
                        break;
                    }
                    if (targetFnName == "va_end" || targetFnName == "llvm.va_end.p0") {
                        if (inst->operands.size() > 1) {
                            void* apPtr = getOperand(inst->operands[1], frame).asPtr();
                            if (apPtr) {
                                activeVaLists.erase(apPtr);
                                void* orig = *reinterpret_cast<void**>(apPtr);
                                if (orig) activeVaLists.erase(orig);
                            }
                        }
                        break;
                    }
                    if (targetFnName == "va_arg") {
                        VMValue retVal(0);
                        if (inst->operands.size() > 1) {
                            void* apPtr = getOperand(inst->operands[1], frame).asPtr();
                            VMVaList* vl = nullptr;
                            auto it = activeVaLists.find(apPtr);
                            if (it != activeVaLists.end()) {
                                vl = &it->second;
                            } else if (apPtr) {
                                void* orig = *reinterpret_cast<void**>(apPtr);
                                auto it2 = activeVaLists.find(orig);
                                if (it2 != activeVaLists.end()) {
                                    vl = &it2->second;
                                }
                            }
                            if (vl && vl->index < vl->args.size()) {
                                retVal = vl->args[vl->index++];
                            }
                        }
                        if (!inst->resultVar.empty()) {
                            frame.registers[inst->resultVar] = retVal;
                        }
                        break;
                    }

                    std::vector<VMValue> callArgs;
                    for (size_t i = 1; i < inst->operands.size(); ++i) {
                        callArgs.push_back(getOperand(inst->operands[i], frame));
                    }

                    VMValue retVal;
                    if (currentModule) {
                        Function* callee = currentModule->getFunction(targetFnName);
                        if (callee) {
                            retVal = executeFunction(callee, callArgs);
                        } else {
                            retVal = callExtern(targetFnName, callArgs, inst->type);
                        }
                    } else {
                        retVal = callExtern(targetFnName, callArgs, inst->type);
                    }

                    if (inst->type && (inst->type->isStruct() || inst->type->isArray() || inst->type->isClosure() || inst->type->isInterface() || inst->type->isSlice())) {
                        size_t sz = 16;
                        if (inst->type->isStruct()) {
                            sz = currentModule ? currentModule->getStructSize(inst->type->nominalName()) : 0;
                            if (sz == 0) sz = getTypeSize(inst->type, currentModule);
                        } else if (inst->type->isArray()) {
                            sz = getTypeSize(inst->type, currentModule);
                        }
                        if (sz == 0) sz = 16;
                        void* callerSlot = frame.allocStack(sz, 8);
                        if (retVal.asPtr()) {
                            std::memcpy(callerSlot, retVal.asPtr(), sz);
                        }
                        retVal = VMValue(callerSlot);
                    }

                    if (!inst->resultVar.empty()) {
                        frame.registers[inst->resultVar] = retVal;
                    }
                    if (hasException()) {
                        if (!frame.exceptionHandlers.empty()) {
                            nextBB = frame.exceptionHandlers.back();
                            frame.exceptionHandlers.pop_back();
                        } else {
                            if (caughtExceptionStack.size() > savedCaughtDepth) caughtExceptionStack.resize(savedCaughtDepth);
                            return VMValue(0);
                        }
                    }
                    break;
                }
                case Opcode::InlineAsm: {
                    // In-VM execution of inline assembly:
                    // For bare-metal / kernel / user inline asm, side-effects like nop, memory
                    // clobbers, or barriers are safe no-ops in VM. If a register output was requested,
                    // provide a default zero value so execution continues deterministically.
                    if (!inst->resultVar.empty()) {
                        frame.registers[inst->resultVar] = VMValue(0);
                    }
                    break;
                }
                default:
                    break;
            }

            // Every integer value is held normalized to its type: wrapped to its width,
            // extended by its signedness.
            if (!inst->resultVar.empty() && inst->type && inst->type->isInt() && inst->type->bitWidth < 64) {
                auto rit = frame.registers.find(inst->resultVar);
                if (rit != frame.registers.end() && rit->second.kind == VMValue::Kind::Int)
                    rit->second.i = normalizeInt(rit->second.i, inst->type);
            }
            if (nextBB) break;
        }

        curBB = nextBB;
    }

    if (caughtExceptionStack.size() > savedCaughtDepth) caughtExceptionStack.resize(savedCaughtDepth);
    return VMValue(0);
}

int VM::run(Module* module, const std::string& entryFn, const std::vector<std::string>& args) {
    if (!module) return 1;
    currentModule = module;

    // Allocate global variables first
    for (const auto& g : module->globals) {
        size_t sz = 0;
        if (g.type && g.type->isStruct()) {
            sz = module->getStructSize(g.type->nominalName());
        }
        if (sz == 0) sz = getTypeSize(g.type, module);
        if (sz < 1) sz = 8;
        globalStorage[g.name].resize(sz, 0);
    }

    // Initialize global variables in second pass (so address-of globals point to allocated storage)
    for (const auto& g : module->globals) {
        if (!g.elements.empty()) {
            StackFrame dummy;
            size_t offset = 0;
            for (const auto& elem : g.elements) {
                VMValue val = getOperand(elem, dummy);
                TypePtr elemTy = elem.type ? elem.type : Type::getPtr();
                size_t elemSz = 0;
                if (elemTy && elemTy->isStruct()) {
                    elemSz = module->getStructSize(elemTy->nominalName());
                }
                if (elemSz == 0) elemSz = getTypeSize(elemTy, module);
                if (offset + elemSz <= globalStorage[g.name].size()) {
                    writeMemory(globalStorage[g.name].data() + offset, elemTy, val);
                }
                offset += elemSz;
            }
        } else if (g.init.kind != OperandKind::ConstantNull) {
            StackFrame dummy;
            VMValue val = getOperand(g.init, dummy);
            writeMemory(globalStorage[g.name].data(), g.type, val);
        }
    }

    if (auto initFn = module->getFunction("__eir_global_init")) {
        executeFunction(initFn, {});
    }

    Function* mainFn = module->getFunction(entryFn);
    if (!mainFn) {
        std::cerr << "error: entry function @" << entryFn << " not found in module" << std::endl;
        return 1;
    }

    std::vector<VMValue> mainArgs;
    std::vector<const char*> argvPointers;
    void* argvBuf = nullptr;
    if (!mainFn->params.empty()) {
        for (const auto& a : args) {
            internedStrings[a] = a;
            argvPointers.push_back(internedStrings[a].c_str());
        }
        argvPointers.push_back(nullptr);

        mainArgs.push_back(VMValue(static_cast<int64_t>(args.size())));
        if (mainFn->params.size() >= 2) {
            argvBuf = malloc(argvPointers.size() * sizeof(const char*));
            std::memcpy(argvBuf, argvPointers.data(), argvPointers.size() * sizeof(const char*));
            mainArgs.push_back(VMValue(argvBuf));
        }
    }

    VMValue res = executeFunction(mainFn, mainArgs);
    if (argvBuf) free(argvBuf);
    if (hasException()) {
        std::cerr << "error: uncaught exception: " << formatException(currentException) << std::endl;
        return 1;
    }
    return static_cast<int>(res.asInt());
}

} // namespace eir
