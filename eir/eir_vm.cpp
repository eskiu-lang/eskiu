#include "eir_vm.h"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <dlfcn.h>

namespace eir {

VM::VM() = default;

void VM::writeMemory(void* ptr, TypePtr type, const VMValue& val) {
    if (!ptr || !type) return;

    if (type->bitWidth == 1 || (type->isInt() && type->bitWidth == 8)) {
        *reinterpret_cast<uint8_t*>(ptr) = static_cast<uint8_t>(val.asInt());
    } else if (type->isInt() && type->bitWidth == 16) {
        *reinterpret_cast<uint16_t*>(ptr) = static_cast<uint16_t>(val.asInt());
    } else if (type->isInt() && (type->bitWidth == 32 || type->bitWidth == 0)) {
        *reinterpret_cast<uint32_t*>(ptr) = static_cast<uint32_t>(val.asInt());
    } else if (type->isInt() && type->bitWidth == 64) {
        *reinterpret_cast<uint64_t*>(ptr) = static_cast<uint64_t>(val.asInt());
    } else if (type->isFloat()) {
        if (type->bitWidth == 32) {
            *reinterpret_cast<float*>(ptr) = static_cast<float>(val.asFloat());
        } else {
            *reinterpret_cast<double*>(ptr) = val.asFloat();
        }
    } else if (type->isPointer()) {
        *reinterpret_cast<void**>(ptr) = val.asPtr();
    } else if (type->isStruct() || type->isArray()) {
        size_t sz = currentModule ? currentModule->getStructSize(type->nominalName()) : 0;
        if (sz == 0) sz = getTypeSize(type);
        if (sz > 0 && val.asPtr()) {
            std::memcpy(ptr, val.asPtr(), sz);
        }
    } else if (type->isClosure() || type->isInterface() || type->isSlice()) {
        if (val.asPtr()) {
            std::memcpy(ptr, val.asPtr(), 16);
        }
    } else {
        *reinterpret_cast<uint64_t*>(ptr) = static_cast<uint64_t>(val.asInt());
    }
}

VMValue VM::readMemory(void* ptr, TypePtr type) {
    if (!ptr || !type) return VMValue(0);

    if (type->bitWidth == 1) {
        return VMValue(static_cast<int64_t>(*reinterpret_cast<uint8_t*>(ptr) != 0 ? 1 : 0));
    }
    if (type->isInt()) {
        if (type->bitWidth == 8) {
            return VMValue(type->isSigned ? static_cast<int64_t>(*reinterpret_cast<int8_t*>(ptr))
                                          : static_cast<int64_t>(*reinterpret_cast<uint8_t*>(ptr)));
        }
        if (type->bitWidth == 16) {
            return VMValue(type->isSigned ? static_cast<int64_t>(*reinterpret_cast<int16_t*>(ptr))
                                          : static_cast<int64_t>(*reinterpret_cast<uint16_t*>(ptr)));
        }
        if (type->bitWidth == 32 || type->bitWidth == 0) {
            return VMValue(type->isSigned ? static_cast<int64_t>(*reinterpret_cast<int32_t*>(ptr))
                                          : static_cast<int64_t>(*reinterpret_cast<uint32_t*>(ptr)));
        }
        if (type->bitWidth == 64) {
            return VMValue(*reinterpret_cast<int64_t*>(ptr));
        }
    }
    if (type->isFloat()) {
        if (type->bitWidth == 32) {
            return VMValue(static_cast<double>(*reinterpret_cast<float*>(ptr)));
        }
        return VMValue(*reinterpret_cast<double*>(ptr));
    }
    if (type->isPointer()) {
        return VMValue(*reinterpret_cast<void**>(ptr));
    }
    if (type->isStruct() || type->isArray() || type->isClosure() || type->isInterface() || type->isSlice()) {
        return VMValue(ptr);
    }
    return VMValue(*reinterpret_cast<int64_t*>(ptr));
}

VMValue VM::getOperand(const Operand& op, StackFrame& frame) {
    switch (op.kind) {
        case OperandKind::Variable: {
            auto it = frame.registers.find(op.text);
            if (it != frame.registers.end()) return it->second;
            return VMValue(0);
        }
        case OperandKind::ConstantInt:
            return VMValue(op.intVal);
        case OperandKind::ConstantFloat:
            return VMValue(op.floatVal);
        case OperandKind::ConstantString: {
            auto it = internedStrings.find(op.text);
            if (it == internedStrings.end()) {
                internedStrings[op.text] = op.text;
                it = internedStrings.find(op.text);
            }
            return VMValue(const_cast<char*>(it->second.c_str()));
        }
        case OperandKind::ConstantNull:
            return VMValue(static_cast<void*>(nullptr));
        case OperandKind::GlobalRef: {
            auto it = globalStorage.find(op.text);
            if (it != globalStorage.end()) {
                return VMValue(static_cast<void*>(it->second.data()));
            }
            // Check if it's a function reference
            if (currentModule) {
                if (auto fn = currentModule->getFunction(op.text)) {
                    return VMValue(static_cast<void*>(fn));
                }
            }
            return VMValue(static_cast<void*>(nullptr));
        }
        case OperandKind::BlockRef:
            return VMValue(0);
    }
    return VMValue(0);
}

std::string VM::formatString(const std::string& fmt, const std::vector<VMValue>& args) {
    std::string out;
    size_t argIdx = 0;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == '%' && i + 1 < fmt.size()) {
            if (fmt[i + 1] == '%') {
                out += '%';
                i++;
                continue;
            }

            size_t start = i;
            i++;
            // Skip flags, width, precision, modifiers
            while (i < fmt.size() && (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' ||
                                      fmt[i] == '#' || fmt[i] == '0' || fmt[i] == '.' ||
                                      (fmt[i] >= '0' && fmt[i] <= '9') ||
                                      fmt[i] == 'l' || fmt[i] == 'h' || fmt[i] == 'z')) {
                i++;
            }

            if (i >= fmt.size() || argIdx >= args.size()) {
                out += fmt.substr(start, i - start + 1);
                continue;
            }

            char conv = fmt[i];
            std::string spec = fmt.substr(start, i - start + 1);
            char buf[256];
            VMValue arg = args[argIdx++];

            if (conv == 'd' || conv == 'i') {
                if (spec.find("ll") == std::string::npos) {
                    spec.insert(spec.size() - 1, "ll");
                }
                std::snprintf(buf, sizeof(buf), spec.c_str(), static_cast<long long>(arg.asInt()));
                out += buf;
            } else if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
                if (spec.find("ll") == std::string::npos) {
                    spec.insert(spec.size() - 1, "ll");
                }
                std::snprintf(buf, sizeof(buf), spec.c_str(), static_cast<unsigned long long>(arg.asInt()));
                out += buf;
            } else if (conv == 'f' || conv == 'g' || conv == 'G' || conv == 'e' || conv == 'E') {
                std::snprintf(buf, sizeof(buf), spec.c_str(), arg.asFloat());
                out += buf;
            } else if (conv == 's') {
                const char* s = static_cast<const char*>(arg.asPtr());
                std::snprintf(buf, sizeof(buf), spec.c_str(), s ? s : "(null)");
                out += buf;
            } else if (conv == 'c') {
                std::snprintf(buf, sizeof(buf), spec.c_str(), static_cast<int>(arg.asInt()));
                out += buf;
            } else if (conv == 'p') {
                std::snprintf(buf, sizeof(buf), spec.c_str(), arg.asPtr());
                out += buf;
            } else {
                out += spec;
            }
        } else {
            out += fmt[i];
        }
    }
    return out;
}

void VM::handlePrintf(const std::string& fmt, const std::vector<VMValue>& args) {
    std::cout << formatString(fmt, args);
    std::cout.flush();
}

VMValue VM::callExtern(const std::string& name, const std::vector<VMValue>& args, TypePtr retTy) {
    if (name == "printf") {
        if (!args.empty()) {
            const char* fmt = static_cast<const char*>(args[0].asPtr());
            std::vector<VMValue> varargs(args.begin() + 1, args.end());
            handlePrintf(fmt ? fmt : "", varargs);
        }
        return VMValue(0);
    }
    if (name == "sprintf") {
        if (args.size() >= 2) {
            char* dest = static_cast<char*>(args[0].asPtr());
            const char* fmt = static_cast<const char*>(args[1].asPtr());
            std::vector<VMValue> varargs(args.begin() + 2, args.end());
            std::string formatted = formatString(fmt ? fmt : "", varargs);
            if (dest) {
                std::memcpy(dest, formatted.c_str(), formatted.size() + 1);
            }
            return VMValue(static_cast<int64_t>(formatted.size()));
        }
        return VMValue(0);
    }
    if (name == "snprintf") {
        if (args.size() >= 3) {
            char* dest = static_cast<char*>(args[0].asPtr());
            size_t maxLen = static_cast<size_t>(args[1].asInt());
            const char* fmt = static_cast<const char*>(args[2].asPtr());
            std::vector<VMValue> varargs(args.begin() + 3, args.end());
            std::string formatted = formatString(fmt ? fmt : "", varargs);
            if (dest && maxLen > 0) {
                size_t toCopy = std::min(maxLen - 1, formatted.size());
                std::memcpy(dest, formatted.c_str(), toCopy);
                dest[toCopy] = '\0';
            }
            return VMValue(static_cast<int64_t>(formatted.size()));
        }
        return VMValue(0);
    }
    if (name == "puts") {
        if (!args.empty()) {
            const char* s = static_cast<const char*>(args[0].asPtr());
            if (s) std::cout << s << std::endl;
        }
        return VMValue(0);
    }
    if (name == "fputs") {
        if (args.size() >= 2) {
            const char* s = static_cast<const char*>(args[0].asPtr());
            FILE* stream = static_cast<FILE*>(args[1].asPtr());
            if (s && stream) fputs(s, stream);
            else if (s) std::cout << s;
        }
        return VMValue(0);
    }
    if (name == "fflush") {
        if (!args.empty()) {
            FILE* stream = static_cast<FILE*>(args[0].asPtr());
            if (stream) fflush(stream);
            else std::cout.flush();
        }
        return VMValue(0);
    }
    if (name == "memcpy") {
        if (args.size() >= 3) {
            memcpy(args[0].asPtr(), args[1].asPtr(), static_cast<size_t>(args[2].asInt()));
            return args[0];
        }
        return VMValue(0);
    }
    if (name == "memmove") {
        if (args.size() >= 3) {
            memmove(args[0].asPtr(), args[1].asPtr(), static_cast<size_t>(args[2].asInt()));
            return args[0];
        }
        return VMValue(0);
    }
    if (name == "memcmp") {
        if (args.size() >= 3) {
            int res = memcmp(args[0].asPtr(), args[1].asPtr(), static_cast<size_t>(args[2].asInt()));
            return VMValue(static_cast<int64_t>(res));
        }
        return VMValue(0);
    }
    if (name == "memset") {
        if (args.size() >= 3) {
            memset(args[0].asPtr(), static_cast<int>(args[1].asInt()), static_cast<size_t>(args[2].asInt()));
            return args[0];
        }
        return VMValue(0);
    }
    if (name == "strlen") {
        if (!args.empty()) {
            const char* s = static_cast<const char*>(args[0].asPtr());
            return VMValue(static_cast<int64_t>(s ? strlen(s) : 0));
        }
        return VMValue(0);
    }
    if (name == "strcmp") {
        if (args.size() >= 2) {
            const char* a = static_cast<const char*>(args[0].asPtr());
            const char* b = static_cast<const char*>(args[1].asPtr());
            return VMValue(static_cast<int64_t>(strcmp(a ? a : "", b ? b : "")));
        }
        return VMValue(0);
    }
    if (name == "calloc") {
        if (args.size() >= 2) {
            void* p = calloc(static_cast<size_t>(args[0].asInt()), static_cast<size_t>(args[1].asInt()));
            return VMValue(p);
        }
        return VMValue(static_cast<void*>(nullptr));
    }
    if (name == "malloc") {
        if (!args.empty()) {
            void* p = malloc(static_cast<size_t>(args[0].asInt()));
            return VMValue(p);
        }
        return VMValue(static_cast<void*>(nullptr));
    }
    if (name == "free") {
        if (!args.empty() && args[0].asPtr()) {
            free(args[0].asPtr());
        }
        return VMValue(0);
    }
    if (name == "exit") {
        int code = args.empty() ? 0 : static_cast<int>(args[0].asInt());
        std::exit(code);
    }
    if (name == "io_stdout") return VMValue(static_cast<void*>(stdout));
    if (name == "io_stderr") return VMValue(static_cast<void*>(stderr));
    if (name == "io_stdin")  return VMValue(static_cast<void*>(stdin));

    // Fallback: dynamic lookup via dlsym
    void* sym = dlsym(RTLD_DEFAULT, name.c_str());
    if (sym) {
        void* res = nullptr;
        switch (args.size()) {
            case 0: { auto f = reinterpret_cast<void*(*)()>(sym); res = f(); break; }
            case 1: { auto f = reinterpret_cast<void*(*)(void*)>(sym); res = f(args[0].asPtr()); break; }
            case 2: { auto f = reinterpret_cast<void*(*)(void*,void*)>(sym); res = f(args[0].asPtr(), args[1].asPtr()); break; }
            case 3: { auto f = reinterpret_cast<void*(*)(void*,void*,void*)>(sym); res = f(args[0].asPtr(), args[1].asPtr(), args[2].asPtr()); break; }
            case 4: { auto f = reinterpret_cast<void*(*)(void*,void*,void*,void*)>(sym); res = f(args[0].asPtr(), args[1].asPtr(), args[2].asPtr(), args[3].asPtr()); break; }
            case 5: { auto f = reinterpret_cast<void*(*)(void*,void*,void*,void*,void*)>(sym); res = f(args[0].asPtr(), args[1].asPtr(), args[2].asPtr(), args[3].asPtr(), args[4].asPtr()); break; }
            case 6: { auto f = reinterpret_cast<void*(*)(void*,void*,void*,void*,void*,void*)>(sym); res = f(args[0].asPtr(), args[1].asPtr(), args[2].asPtr(), args[3].asPtr(), args[4].asPtr(), args[5].asPtr()); break; }
            default:
                std::cerr << "error: VM extern call with >6 arguments not supported: " << name << std::endl;
                return VMValue(0);
        }
        if (retTy && retTy->isPointer()) return VMValue(res);
        return VMValue(reinterpret_cast<int64_t>(res));
    }

    std::cerr << "warning: unresolved external function in VM: " << name << std::endl;
    return VMValue(0);
}

VMValue VM::executeFunction(Function* fn, const std::vector<VMValue>& args) {
    if (!fn) return VMValue(0);
    if (fn->isExtern) {
        return callExtern(fn->name, args, fn->returnType);
    }

    StackFrame frame;
    frame.function = fn;

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
                    if (sz == 0) sz = getTypeSize(inst->type);
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
                    size_t elemSize = getTypeSize(elemType);
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
                    frame.registers[inst->resultVar] = VMValue(a + b);
                    break;
                }
                case Opcode::Sub: {
                    int64_t a = getOperand(inst->operands[0], frame).asInt();
                    int64_t b = getOperand(inst->operands[1], frame).asInt();
                    frame.registers[inst->resultVar] = VMValue(a - b);
                    break;
                }
                case Opcode::Mul: {
                    int64_t a = getOperand(inst->operands[0], frame).asInt();
                    int64_t b = getOperand(inst->operands[1], frame).asInt();
                    frame.registers[inst->resultVar] = VMValue(a * b);
                    break;
                }
                case Opcode::SDiv: {
                    int64_t a = getOperand(inst->operands[0], frame).asInt();
                    int64_t b = getOperand(inst->operands[1], frame).asInt();
                    frame.registers[inst->resultVar] = VMValue(b != 0 ? a / b : 0);
                    break;
                }
                case Opcode::UDiv: {
                    uint64_t a = static_cast<uint64_t>(getOperand(inst->operands[0], frame).asInt());
                    uint64_t b = static_cast<uint64_t>(getOperand(inst->operands[1], frame).asInt());
                    frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(b != 0 ? a / b : 0));
                    break;
                }
                case Opcode::SRem: {
                    int64_t a = getOperand(inst->operands[0], frame).asInt();
                    int64_t b = getOperand(inst->operands[1], frame).asInt();
                    frame.registers[inst->resultVar] = VMValue(b != 0 ? a % b : 0);
                    break;
                }
                case Opcode::URem: {
                    uint64_t a = static_cast<uint64_t>(getOperand(inst->operands[0], frame).asInt());
                    uint64_t b = static_cast<uint64_t>(getOperand(inst->operands[1], frame).asInt());
                    frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(b != 0 ? a % b : 0));
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
                    frame.registers[inst->resultVar] = VMValue(b != 0.0 ? a / b : 0.0);
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
                    frame.registers[inst->resultVar] = VMValue(a & b);
                    break;
                }
                case Opcode::Or: {
                    int64_t a = getOperand(inst->operands[0], frame).asInt();
                    int64_t b = getOperand(inst->operands[1], frame).asInt();
                    frame.registers[inst->resultVar] = VMValue(a | b);
                    break;
                }
                case Opcode::Xor: {
                    int64_t a = getOperand(inst->operands[0], frame).asInt();
                    int64_t b = getOperand(inst->operands[1], frame).asInt();
                    frame.registers[inst->resultVar] = VMValue(a ^ b);
                    break;
                }
                case Opcode::Shl: {
                    int64_t a = getOperand(inst->operands[0], frame).asInt();
                    int64_t b = getOperand(inst->operands[1], frame).asInt();
                    if (inst->type && inst->type->bitWidth == 8) {
                        uint8_t ua = static_cast<uint8_t>(a);
                        frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(static_cast<uint8_t>(ua << b)));
                    } else if (inst->type && inst->type->bitWidth == 16) {
                        uint16_t ua = static_cast<uint16_t>(a);
                        frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(static_cast<uint16_t>(ua << b)));
                    } else if (inst->type && inst->type->bitWidth == 32) {
                        uint32_t ua = static_cast<uint32_t>(a);
                        frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(static_cast<int32_t>(ua << b)));
                    } else {
                        frame.registers[inst->resultVar] = VMValue(a << b);
                    }
                    break;
                }
                case Opcode::LShr: {
                    uint64_t a = static_cast<uint64_t>(getOperand(inst->operands[0], frame).asInt());
                    uint64_t b = static_cast<uint64_t>(getOperand(inst->operands[1], frame).asInt());
                    frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(a >> b));
                    break;
                }
                case Opcode::AShr: {
                    int64_t a = getOperand(inst->operands[0], frame).asInt();
                    int64_t b = getOperand(inst->operands[1], frame).asInt();
                    if (inst->type && inst->type->bitWidth == 8) {
                        int8_t sa = static_cast<int8_t>(a);
                        frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(sa >> b));
                    } else if (inst->type && inst->type->bitWidth == 16) {
                        int16_t sa = static_cast<int16_t>(a);
                        frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(sa >> b));
                    } else if (inst->type && inst->type->bitWidth == 32) {
                        int32_t sa = static_cast<int32_t>(a);
                        frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(sa >> b));
                    } else {
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
                case Opcode::Trunc:
                case Opcode::ZExt:
                case Opcode::SExt:
                case Opcode::Bitcast: {
                    VMValue a = getOperand(inst->operands[0], frame);
                    frame.registers[inst->resultVar] = a;
                    break;
                }
                case Opcode::FPToSI: {
                    double a = getOperand(inst->operands[0], frame).asFloat();
                    frame.registers[inst->resultVar] = VMValue(static_cast<int64_t>(a));
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
                    if (inst->operands.empty() || inst->operands[0].type->isVoid()) {
                        return VMValue(0);
                    }
                    VMValue retVal = getOperand(inst->operands[0], frame);
                    TypePtr retTy = inst->operands[0].type;
                    if (retTy && (retTy->isStruct() || retTy->isArray() || retTy->isClosure() || retTy->isInterface() || retTy->isSlice())) {
                        size_t sz = 16;
                        if (retTy->isStruct()) {
                            sz = currentModule ? currentModule->getStructSize(retTy->nominalName()) : 0;
                            if (sz == 0) sz = getTypeSize(retTy);
                        } else if (retTy->isArray()) {
                            sz = getTypeSize(retTy);
                        }
                        if (sz == 0) sz = 16;
                        returnBuffer.resize(sz);
                        if (retVal.asPtr()) {
                            std::memcpy(returnBuffer.data(), retVal.asPtr(), sz);
                        } else {
                            std::memset(returnBuffer.data(), 0, sz);
                        }
                        return VMValue(returnBuffer.data());
                    }
                    return retVal;
                }
                case Opcode::Unreachable:
                    return VMValue(0);

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
                            if (sz == 0) sz = getTypeSize(inst->type);
                        } else if (inst->type->isArray()) {
                            sz = getTypeSize(inst->type);
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
                    break;
                }

                case Opcode::Call: {
                    std::string targetFnName = inst->operands[0].text;
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
                            if (sz == 0) sz = getTypeSize(inst->type);
                        } else if (inst->type->isArray()) {
                            sz = getTypeSize(inst->type);
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

            if (nextBB) break;
        }

        curBB = nextBB;
    }

    return VMValue(0);
}

int VM::run(Module* module, const std::string& entryFn, const std::vector<std::string>& /*args*/) {
    if (!module) return 1;
    currentModule = module;

    // Allocate global variables first
    for (const auto& g : module->globals) {
        size_t sz = getTypeSize(g.type);
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
                size_t elemSz = getTypeSize(elemTy);
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

    VMValue res = executeFunction(mainFn, {});
    return static_cast<int>(res.asInt());
}

} // namespace eir
