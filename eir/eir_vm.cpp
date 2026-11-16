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
        if (sz == 0) sz = getTypeSize(type, currentModule);
        if (sz > 0 && val.asPtr()) {
            std::memcpy(ptr, val.asPtr(), sz);
        }
    } else if (type->isClosure() || type->isInterface() || type->isSlice()) {
        if (val.asPtr()) {
            std::memcpy(ptr, val.asPtr(), 16);
        } else {
            std::memset(ptr, 0, 16);
        }
    } else {
        *reinterpret_cast<uint64_t*>(ptr) = static_cast<uint64_t>(val.asInt());
    }
}

int64_t normalizeInt(int64_t v, const TypePtr& t) {
    if (!t || !t->isInt()) return v;
    switch (t->bitWidth) {
        case 1: return v & 1;
        case 8: return t->isSigned ? static_cast<int64_t>(static_cast<int8_t>(v)) : static_cast<int64_t>(static_cast<uint8_t>(v));
        case 16: return t->isSigned ? static_cast<int64_t>(static_cast<int16_t>(v)) : static_cast<int64_t>(static_cast<uint16_t>(v));
        case 32: return t->isSigned ? static_cast<int64_t>(static_cast<int32_t>(v)) : static_cast<int64_t>(static_cast<uint32_t>(v));
        default: return v;
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

            // Flags: '-', '+', ' ', '#', '0'
            while (i < fmt.size() && (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' || fmt[i] == '#' || fmt[i] == '0')) {
                i++;
            }

            // Width: '*' or digits
            bool hasWidthArg = false;
            int widthArg = 0;
            if (i < fmt.size() && fmt[i] == '*') {
                hasWidthArg = true;
                if (argIdx < args.size()) widthArg = static_cast<int>(args[argIdx++].asInt());
                i++;
            } else {
                while (i < fmt.size() && (fmt[i] >= '0' && fmt[i] <= '9')) i++;
            }

            // Precision: '.' followed by '*' or digits
            bool hasPrecArg = false;
            int precArg = 0;
            if (i < fmt.size() && fmt[i] == '.') {
                i++;
                if (i < fmt.size() && fmt[i] == '*') {
                    hasPrecArg = true;
                    if (argIdx < args.size()) precArg = static_cast<int>(args[argIdx++].asInt());
                    i++;
                } else {
                    while (i < fmt.size() && (fmt[i] >= '0' && fmt[i] <= '9')) i++;
                }
            }

            // Length modifiers: 'l', 'h', 'z', etc.
            while (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'h' || fmt[i] == 'z' || fmt[i] == 'j' || fmt[i] == 't')) {
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
                long long val = static_cast<long long>(arg.asInt());
                if (hasWidthArg && hasPrecArg) {
                    std::snprintf(buf, sizeof(buf), spec.c_str(), widthArg, precArg, val);
                } else if (hasWidthArg) {
                    std::snprintf(buf, sizeof(buf), spec.c_str(), widthArg, val);
                } else if (hasPrecArg) {
                    std::snprintf(buf, sizeof(buf), spec.c_str(), precArg, val);
                } else {
                    std::snprintf(buf, sizeof(buf), spec.c_str(), val);
                }
                out += buf;
            } else if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
                if (spec.find("ll") == std::string::npos) {
                    spec.insert(spec.size() - 1, "ll");
                }
                unsigned long long val = static_cast<unsigned long long>(arg.asInt());
                if (hasWidthArg && hasPrecArg) {
                    std::snprintf(buf, sizeof(buf), spec.c_str(), widthArg, precArg, val);
                } else if (hasWidthArg) {
                    std::snprintf(buf, sizeof(buf), spec.c_str(), widthArg, val);
                } else if (hasPrecArg) {
                    std::snprintf(buf, sizeof(buf), spec.c_str(), precArg, val);
                } else {
                    std::snprintf(buf, sizeof(buf), spec.c_str(), val);
                }
                out += buf;
            } else if (conv == 'f' || conv == 'g' || conv == 'G' || conv == 'e' || conv == 'E') {
                double val = arg.asFloat();
                if (hasWidthArg && hasPrecArg) {
                    std::snprintf(buf, sizeof(buf), spec.c_str(), widthArg, precArg, val);
                } else if (hasWidthArg) {
                    std::snprintf(buf, sizeof(buf), spec.c_str(), widthArg, val);
                } else if (hasPrecArg) {
                    std::snprintf(buf, sizeof(buf), spec.c_str(), precArg, val);
                } else {
                    std::snprintf(buf, sizeof(buf), spec.c_str(), val);
                }
                out += buf;
            } else if (conv == 's') {
                const char* s = static_cast<const char*>(arg.asPtr());
                if (!s) s = "(null)";
                if (hasWidthArg && hasPrecArg) {
                    std::vector<char> dyn(std::abs(widthArg) + std::abs(precArg) + 64);
                    std::snprintf(dyn.data(), dyn.size(), spec.c_str(), widthArg, precArg, s);
                    out += dyn.data();
                } else if (hasWidthArg) {
                    std::vector<char> dyn(std::abs(widthArg) + std::strlen(s) + 64);
                    std::snprintf(dyn.data(), dyn.size(), spec.c_str(), widthArg, s);
                    out += dyn.data();
                } else if (hasPrecArg) {
                    if (precArg < 0) precArg = 0;
                    size_t slen = 0;
                    while (slen < static_cast<size_t>(precArg) && s[slen] != '\0') slen++;
                    out.append(s, slen);
                } else {
                    if (spec == "%s") {
                        out += s;
                    } else {
                        size_t sLen = std::strlen(s);
                        std::vector<char> dynBuf(sLen + 64);
                        std::snprintf(dynBuf.data(), dynBuf.size(), spec.c_str(), s);
                        out += dynBuf.data();
                    }
                }
            } else if (conv == 'c') {
                int val = static_cast<int>(arg.asInt());
                if (hasWidthArg) {
                    std::snprintf(buf, sizeof(buf), spec.c_str(), widthArg, val);
                } else {
                    std::snprintf(buf, sizeof(buf), spec.c_str(), val);
                }
                out += buf;
            } else if (conv == 'p') {
                void* val = arg.asPtr();
                if (hasWidthArg) {
                    std::snprintf(buf, sizeof(buf), spec.c_str(), widthArg, val);
                } else {
                    std::snprintf(buf, sizeof(buf), spec.c_str(), val);
                }
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

std::string VM::formatException(void* exPtr) {
    if (!exPtr) return "unknown";
    char* typeName = *reinterpret_cast<char**>(static_cast<char*>(exPtr) + 8);
    void* payload = static_cast<char*>(exPtr) + 16;
    if (!typeName) return "unknown";
    std::string tn(typeName);

    if (tn == "int" || tn == "int32") {
        int32_t val = *reinterpret_cast<int32_t*>(payload);
        return tn + " " + std::to_string(val);
    } else if (tn == "int8") {
        int8_t val = *reinterpret_cast<int8_t*>(payload);
        return tn + " " + std::to_string(static_cast<int>(val));
    } else if (tn == "int16") {
        int16_t val = *reinterpret_cast<int16_t*>(payload);
        return tn + " " + std::to_string(val);
    } else if (tn == "int64") {
        int64_t val = *reinterpret_cast<int64_t*>(payload);
        return tn + " " + std::to_string(val);
    } else if (tn == "uint" || tn == "uint32") {
        uint32_t val = *reinterpret_cast<uint32_t*>(payload);
        return tn + " " + std::to_string(val);
    } else if (tn == "uint8") {
        uint8_t val = *reinterpret_cast<uint8_t*>(payload);
        return tn + " " + std::to_string(static_cast<unsigned>(val));
    } else if (tn == "uint16") {
        uint16_t val = *reinterpret_cast<uint16_t*>(payload);
        return tn + " " + std::to_string(val);
    } else if (tn == "uint64") {
        uint64_t val = *reinterpret_cast<uint64_t*>(payload);
        return tn + " " + std::to_string(val);
    } else if (tn == "double") {
        double val = *reinterpret_cast<double*>(payload);
        char buf[64];
        snprintf(buf, sizeof(buf), "%g", val);
        return tn + " " + buf;
    } else if (tn == "float") {
        float val = *reinterpret_cast<float*>(payload);
        char buf[64];
        snprintf(buf, sizeof(buf), "%g", val);
        return tn + " " + buf;
    } else if (tn == "bool") {
        bool val = (*reinterpret_cast<uint8_t*>(payload)) != 0;
        return tn + " " + (val ? "true" : "false");
    } else if (tn == "char") {
        char val = *reinterpret_cast<char*>(payload);
        std::string s = tn + " '";
        s += val;
        s += "'";
        return s;
    } else if (tn == "string") {
        char* str = *reinterpret_cast<char**>(payload);
        if (str) {
            return tn + " \"" + std::string(str) + "\"";
        } else {
            return tn;
        }
    } else {
        return tn;
    }
}

} // namespace eir
