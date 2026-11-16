// eir_vm_exec.cpp: the interpreter loop (executeFunction), its control-flow and call
// instructions, and run (split from eir_vm.cpp; the other instructions are in eir_vm_ops.cpp).
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
                case Opcode::Alloca: case Opcode::Load: case Opcode::Store: case Opcode::Gep:
                case Opcode::FieldPtr: case Opcode::OffsetPtr:
                    execMemory(inst, frame);
                    break;
                case Opcode::Add: case Opcode::Sub: case Opcode::Mul: case Opcode::SDiv: case Opcode::UDiv:
                case Opcode::SRem: case Opcode::URem:
                    execIntArith(inst, frame);
                    break;
                case Opcode::FAdd: case Opcode::FSub: case Opcode::FMul: case Opcode::FDiv:
                case Opcode::Neg:
                    execFloatArith(inst, frame);
                    break;
                case Opcode::And: case Opcode::Or: case Opcode::Xor: case Opcode::Shl: case Opcode::LShr:
                case Opcode::AShr: case Opcode::Not:
                    execBitwise(inst, frame);
                    break;
                case Opcode::ICmp: case Opcode::FCmp:
                    execCompare(inst, frame);
                    break;
                case Opcode::Trunc: case Opcode::ZExt: case Opcode::SExt: case Opcode::Bitcast:
                case Opcode::PtrToInt: case Opcode::IntToPtr: case Opcode::FPTrunc: case Opcode::FPExt:
                case Opcode::FPToSI: case Opcode::SIToFP:
                    execConvert(inst, frame);
                    break;
                case Opcode::Br: case Opcode::BrCond: case Opcode::Switch:
                    execBranch(fn, inst, frame, nextBB);
                    break;
                case Opcode::Ret: case Opcode::Unreachable: case Opcode::Throw:
                    if (auto r = execReturn(inst, frame, nextBB, savedCaughtDepth)) return *r;
                    break;
                case Opcode::PushHandler: case Opcode::PopHandler: case Opcode::LandingPad:
                case Opcode::MakeClosure: case Opcode::MakeInterface: case Opcode::MakeSlice:
                case Opcode::ExtractFat:
                    execHandlers(inst, frame);
                    break;
                case Opcode::CallIndirect:
                    if (auto r = execCallIndirect(inst, frame, nextBB, savedCaughtDepth)) return *r;
                    break;
                case Opcode::Call:
                    if (auto r = execCall(inst, frame, nextBB, savedCaughtDepth)) return *r;
                    break;
                case Opcode::InlineAsm: default:
                    execInlineAsm(inst, frame);
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

void VM::execBranch(Function* fn, const std::unique_ptr<Instruction>& inst, StackFrame& frame, BasicBlock*& nextBB) {
    switch (inst->op) {
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
        default:
            break;
    }
}

std::optional<VMValue> VM::execReturn(const std::unique_ptr<Instruction>& inst, StackFrame& frame, BasicBlock*& nextBB, size_t savedCaughtDepth) {
    switch (inst->op) {
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
        default:
            break;
    }
    return std::nullopt;
}

void VM::execHandlers(const std::unique_ptr<Instruction>& inst, StackFrame& frame) {
    switch (inst->op) {
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
        default:
            break;
    }
}

std::optional<VMValue> VM::execCallIndirect(const std::unique_ptr<Instruction>& inst, StackFrame& frame, BasicBlock*& nextBB, size_t savedCaughtDepth) {
    switch (inst->op) {
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
        default:
            break;
    }
    return std::nullopt;
}

std::optional<VMValue> VM::execCall(const std::unique_ptr<Instruction>& inst, StackFrame& frame, BasicBlock*& nextBB, size_t savedCaughtDepth) {
    switch (inst->op) {
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
        default:
            break;
    }
    return std::nullopt;
}

void VM::execInlineAsm(const std::unique_ptr<Instruction>& inst, StackFrame& frame) {
    switch (inst->op) {
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
