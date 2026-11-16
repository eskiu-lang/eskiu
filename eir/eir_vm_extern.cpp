// eir_vm_extern.cpp: process symbols, operands and calls to C functions (split from eir_vm.cpp).
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

// A symbol of the running process (the C library included), or null, on every host.
static void* processSymbol(const std::string& name) {
    static bool loaded = !llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
    (void)loaded;
    return llvm::sys::DynamicLibrary::SearchForAddressOfSymbol(name);
}

VMValue VM::getOperand(const Operand& op, StackFrame& frame) {
    switch (op.kind) {
        case OperandKind::Variable: {
            auto it = frame.registers.find(op.text);
            if (it != frame.registers.end()) return it->second;
            return VMValue(0);
        }
        case OperandKind::ConstantInt:
            return VMValue(normalizeInt(op.intVal, op.type));
        case OperandKind::ConstantFloat:
            return VMValue(op.floatVal);
        case OperandKind::ConstantString: {
            static std::mutex internMutex;
            std::lock_guard<std::mutex> lock(internMutex);
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
            if (void* sym = processSymbol(op.text)) {
                return VMValue(sym);
            }
            return VMValue(static_cast<void*>(nullptr));
        }
        case OperandKind::BlockRef:
            return VMValue(0);
    }
    return VMValue(0);
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
    if (name == "vsnprintf" || name == "__mingw_vsnprintf") {
        if (args.size() >= 4) {
            char* dest = static_cast<char*>(args[0].asPtr());
            size_t maxLen = static_cast<size_t>(args[1].asInt());
            const char* fmt = static_cast<const char*>(args[2].asPtr());
            void* apPtr = args[3].asPtr();

            const std::vector<VMValue>* varargs = nullptr;
            auto it = activeVaLists.find(apPtr);
            if (it != activeVaLists.end()) {
                varargs = &it->second.args;
            } else if (apPtr) {
                void* orig = *reinterpret_cast<void**>(apPtr);
                auto it2 = activeVaLists.find(orig);
                if (it2 != activeVaLists.end()) {
                    varargs = &it2->second.args;
                }
            }

            std::string formatted = varargs ? formatString(fmt ? fmt : "", *varargs)
                                            : formatString(fmt ? fmt : "", {});
            if (dest && maxLen > 0) {
                size_t toCopy = std::min(maxLen - 1, formatted.size());
                std::memcpy(dest, formatted.c_str(), toCopy);
                dest[toCopy] = '\0';
            }
            return VMValue(static_cast<int64_t>(formatted.size()));
        }
        return VMValue(0);
    }
    if (name == "strtod") {
        if (args.size() >= 2) {
            const char* str = static_cast<const char*>(args[0].asPtr());
            char** endptr = static_cast<char**>(args[1].asPtr());
            double res = std::strtod(str ? str : "", endptr);
            return VMValue(res);
        }
        return VMValue(0.0);
    }
    if (name == "atof") {
        if (!args.empty()) {
            const char* str = static_cast<const char*>(args[0].asPtr());
            return VMValue(std::atof(str ? str : ""));
        }
        return VMValue(0.0);
    }
    if (name == "sqrt") {
        if (!args.empty()) return VMValue(std::sqrt(args[0].asFloat()));
        return VMValue(0.0);
    }
    if (name == "fabs") {
        if (!args.empty()) return VMValue(std::fabs(args[0].asFloat()));
        return VMValue(0.0);
    }
    if (name == "pow") {
        if (args.size() >= 2) return VMValue(std::pow(args[0].asFloat(), args[1].asFloat()));
        return VMValue(0.0);
    }
    if (name == "floor") {
        if (!args.empty()) return VMValue(std::floor(args[0].asFloat()));
        return VMValue(0.0);
    }
    if (name == "ceil") {
        if (!args.empty()) return VMValue(std::ceil(args[0].asFloat()));
        return VMValue(0.0);
    }
    if (name == "round") {
        if (!args.empty()) return VMValue(std::round(args[0].asFloat()));
        return VMValue(0.0);
    }
    if (name == "trunc") {
        if (!args.empty()) return VMValue(std::trunc(args[0].asFloat()));
        return VMValue(0.0);
    }
    if (name == "fmod") {
        if (args.size() >= 2) return VMValue(std::fmod(args[0].asFloat(), args[1].asFloat()));
        return VMValue(0.0);
    }
    if (name == "sin") {
        if (!args.empty()) return VMValue(std::sin(args[0].asFloat()));
        return VMValue(0.0);
    }
    if (name == "cos") {
        if (!args.empty()) return VMValue(std::cos(args[0].asFloat()));
        return VMValue(0.0);
    }
    if (name == "tan") {
        if (!args.empty()) return VMValue(std::tan(args[0].asFloat()));
        return VMValue(0.0);
    }
    if (name == "asin") {
        if (!args.empty()) return VMValue(std::asin(args[0].asFloat()));
        return VMValue(0.0);
    }
    if (name == "acos") {
        if (!args.empty()) return VMValue(std::acos(args[0].asFloat()));
        return VMValue(0.0);
    }
    if (name == "atan") {
        if (!args.empty()) return VMValue(std::atan(args[0].asFloat()));
        return VMValue(0.0);
    }
    if (name == "atan2") {
        if (args.size() >= 2) return VMValue(std::atan2(args[0].asFloat(), args[1].asFloat()));
        return VMValue(0.0);
    }
    if (name == "log") {
        if (!args.empty()) return VMValue(std::log(args[0].asFloat()));
        return VMValue(0.0);
    }
    if (name == "log2") {
        if (!args.empty()) return VMValue(std::log2(args[0].asFloat()));
        return VMValue(0.0);
    }
    if (name == "log10") {
        if (!args.empty()) return VMValue(std::log10(args[0].asFloat()));
        return VMValue(0.0);
    }
    if (name == "exp") {
        if (!args.empty()) return VMValue(std::exp(args[0].asFloat()));
        return VMValue(0.0);
    }
    if (name == "abs") {
        if (!args.empty()) return VMValue(std::abs(args[0].asInt()));
        return VMValue(0);
    }
    if (name == "bsearch") {
        if (args.size() >= 5) {
            void* key = args[0].asPtr();
            const char* base = static_cast<const char*>(args[1].asPtr());
            size_t n = static_cast<size_t>(args[2].asInt());
            size_t size = static_cast<size_t>(args[3].asInt());
            void* cmpFnPtr = args[4].asPtr();
            Function* cmpFn = static_cast<Function*>(cmpFnPtr);
            size_t l = 0, r = n;
            while (l < r) {
                size_t m = l + (r - l) / 2;
                const void* elem = base + m * size;
                VMValue cres(0);
                if (cmpFn) {
                    cres = executeFunction(cmpFn, {VMValue(const_cast<void*>(key)), VMValue(const_cast<void*>(elem))});
                }
                int cmp = static_cast<int>(cres.asInt());
                if (cmp == 0) return VMValue(const_cast<void*>(elem));
                if (cmp < 0) r = m;
                else l = m + 1;
            }
        }
        return VMValue(static_cast<void*>(nullptr));
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

#ifndef _WIN32
    if (name == "fcntl") {
        if (args.size() == 2) {
            int res = fcntl(static_cast<int>(args[0].asInt()), static_cast<int>(args[1].asInt()));
            return VMValue(static_cast<int64_t>(res));
        }
        if (args.size() >= 3) {
            int res = fcntl(static_cast<int>(args[0].asInt()), static_cast<int>(args[1].asInt()), args[2].asPtr());
            return VMValue(static_cast<int64_t>(res));
        }
        return VMValue(-1);
    }
    if (name == "open") {
        if (args.size() == 2) {
            int res = open(static_cast<const char*>(args[0].asPtr()), static_cast<int>(args[1].asInt()));
            return VMValue(static_cast<int64_t>(res));
        }
        if (args.size() >= 3) {
            int res = open(static_cast<const char*>(args[0].asPtr()), static_cast<int>(args[1].asInt()), static_cast<mode_t>(args[2].asInt()));
            return VMValue(static_cast<int64_t>(res));
        }
        return VMValue(-1);
    }
    if (name == "openat") {
        if (args.size() == 3) {
            int res = openat(static_cast<int>(args[0].asInt()), static_cast<const char*>(args[1].asPtr()), static_cast<int>(args[2].asInt()));
            return VMValue(static_cast<int64_t>(res));
        }
        if (args.size() >= 4) {
            int res = openat(static_cast<int>(args[0].asInt()), static_cast<const char*>(args[1].asPtr()), static_cast<int>(args[2].asInt()), static_cast<mode_t>(args[3].asInt()));
            return VMValue(static_cast<int64_t>(res));
        }
        return VMValue(-1);
    }
    if (name == "ioctl") {
        if (args.size() == 2) {
            int res = ioctl(static_cast<int>(args[0].asInt()), static_cast<unsigned long>(args[1].asInt()));
            return VMValue(static_cast<int64_t>(res));
        }
        if (args.size() >= 3) {
            int res = ioctl(static_cast<int>(args[0].asInt()), static_cast<unsigned long>(args[1].asInt()), args[2].asPtr());
            return VMValue(static_cast<int64_t>(res));
        }
        return VMValue(-1);
    }
#endif
    if (name == "__eir_thread_create") {
        void* target = args.empty() ? nullptr : args[0].asPtr();
        if (!target) return VMValue(static_cast<void*>(nullptr));

        bool owned = args.size() > 1 ? args[1].asInt() != 0 : false;

        Function* callee = nullptr;
        void* envPtr = nullptr;

        // Check if target is a direct Function* pointer
        if (currentModule) {
            for (const auto& f : currentModule->functions) {
                if (f.get() == static_cast<Function*>(target)) {
                    callee = f.get();
                    break;
                }
            }
        }

        // If not a direct Function*, it is a fat pointer struct {fnPtr, envPtr}
        if (!callee) {
            void* fnPtr = *reinterpret_cast<void**>(static_cast<uint8_t*>(target) + 0);
            envPtr = *reinterpret_cast<void**>(static_cast<uint8_t*>(target) + 8);
            if (currentModule) {
                for (const auto& f : currentModule->functions) {
                    if (f.get() == static_cast<Function*>(fnPtr)) {
                        callee = f.get();
                        break;
                    }
                }
            }
            if (!callee) callee = static_cast<Function*>(fnPtr);
        }

        std::vector<VMValue> callArgs;
        if (callee && !callee->params.empty() && callee->params[0].first == "env") {
            callArgs.push_back(VMValue(envPtr));
        }

        auto th = new std::thread([this, callee, callArgs, owned, envPtr]() {
            if (callee) {
                this->executeFunction(callee, callArgs);
                if (this->hasException()) {
                    std::cerr << "error: uncaught exception: " << this->formatException(this->currentException) << std::endl;
                    std::exit(1);
                }
            }
            if (owned && envPtr) {
                free(envPtr);
            }
        });
        return VMValue(static_cast<void*>(th));
    }
    if (name == "__eir_thread_join") {
        if (!args.empty() && args[0].asPtr()) {
            auto th = static_cast<std::thread*>(args[0].asPtr());
            if (th->joinable()) {
                th->join();
            }
            delete th;
        }
        return VMValue(0);
    }
    if (name == "__eir_alloc_exception") {
        int64_t sz = args.size() > 0 ? args[0].asInt() : 16;
        void* typeNamePtr = args.size() > 1 ? args[1].asPtr() : nullptr;
        void* ex = malloc(static_cast<size_t>(sz));
        std::memset(ex, 0, static_cast<size_t>(sz));
        *reinterpret_cast<int64_t*>(ex) = sz;
        *reinterpret_cast<void**>(static_cast<char*>(ex) + 8) = typeNamePtr;
        return VMValue(ex);
    }
    if (name == "__eir_free_exception") {
        if (!args.empty()) free(args[0].asPtr());
        return VMValue(0);
    }
    if (name == "__eir_push_caught") {
        if (!args.empty()) {
            caughtExceptionStack.push_back(args[0].asPtr());
        }
        return VMValue(0);
    }
    if (name == "__eir_pop_caught") {
        if (!caughtExceptionStack.empty()) {
            caughtExceptionStack.pop_back();
        }
        return VMValue(0);
    }
    if (name == "__eir_clear_exception") {
        currentException = nullptr;
        return VMValue(0);
    }
    if (name == "__eir_exception_text") {
        void* ex = nullptr;
        if (!caughtExceptionStack.empty()) {
            ex = caughtExceptionStack.back();
        } else if (currentException) {
            ex = currentException;
        }
        if (ex) {
            activeExceptionText = formatException(ex);
        } else {
            activeExceptionText = "";
        }
        return VMValue(const_cast<char*>(activeExceptionText.c_str()));
    }
    if (name == "__exception_text_into") {
        if (!args.empty() && args[0].asPtr()) {
            char* dst = reinterpret_cast<char*>(args[0].asPtr());
            void* ex = nullptr;
            if (!caughtExceptionStack.empty()) {
                ex = caughtExceptionStack.back();
            } else if (currentException) {
                ex = currentException;
            }
            std::string text = ex ? formatException(ex) : "";
            size_t copyLen = std::min(text.size(), static_cast<size_t>(511));
            std::memcpy(dst, text.data(), copyLen);
            dst[copyLen] = '\0';
        }
        return VMValue(0);
    }
    if (name == "atomic_cas" || name == "atomic_cas_relaxed") {
        if (args.size() >= 3 && args[0].asPtr()) {
            int32_t* cell = reinterpret_cast<int32_t*>(args[0].asPtr());
            int32_t expected = static_cast<int32_t>(args[1].asInt());
            int32_t desired = static_cast<int32_t>(args[2].asInt());
            if (*cell == expected) {
                *cell = desired;
                return VMValue(static_cast<uint64_t>(1));
            } else {
                return VMValue(static_cast<uint64_t>(0));
            }
        }
        return VMValue(0);
    }
    if (name == "atomic_cas64" || name == "atomic_cas64_relaxed" || name == "atomic_cas_ptr" || name == "atomic_cas_ptr_relaxed") {
        if (args.size() >= 3 && args[0].asPtr()) {
            uint64_t* cell = reinterpret_cast<uint64_t*>(args[0].asPtr());
            uint64_t expected = args[1].asInt();
            uint64_t desired = args[2].asInt();
            if (*cell == expected) {
                *cell = desired;
                return VMValue(static_cast<uint64_t>(1));
            } else {
                return VMValue(static_cast<uint64_t>(0));
            }
        }
        return VMValue(0);
    }
    if (name == "atomic_swap" || name == "atomic_swap_relaxed") {
        if (args.size() >= 2 && args[0].asPtr()) {
            int32_t* cell = reinterpret_cast<int32_t*>(args[0].asPtr());
            int32_t desired = static_cast<int32_t>(args[1].asInt());
            int32_t old = *cell;
            *cell = desired;
            return VMValue(static_cast<int64_t>(old));
        }
        return VMValue(0);
    }
    if (name == "atomic_swap64" || name == "atomic_swap64_relaxed" || name == "atomic_swap_ptr" || name == "atomic_swap_ptr_relaxed") {
        if (args.size() >= 2 && args[0].asPtr()) {
            uint64_t* cell = reinterpret_cast<uint64_t*>(args[0].asPtr());
            uint64_t desired = args[1].asInt();
            uint64_t old = *cell;
            *cell = desired;
            return VMValue(old);
        }
        return VMValue(0);
    }
    if (name == "atomic_load" || name == "atomic_load_relaxed") {
        if (!args.empty() && args[0].asPtr()) {
            return VMValue(static_cast<int64_t>(*reinterpret_cast<int32_t*>(args[0].asPtr())));
        }
        return VMValue(0);
    }
    if (name == "atomic_load64" || name == "atomic_load64_relaxed" || name == "atomic_load_ptr" || name == "atomic_load_ptr_relaxed") {
        if (!args.empty() && args[0].asPtr()) {
            return VMValue(*reinterpret_cast<uint64_t*>(args[0].asPtr()));
        }
        return VMValue(0);
    }
    if (name == "atomic_store" || name == "atomic_store_relaxed") {
        if (args.size() >= 2 && args[0].asPtr()) {
            *reinterpret_cast<int32_t*>(args[0].asPtr()) = static_cast<int32_t>(args[1].asInt());
        }
        return VMValue(0);
    }
    if (name == "atomic_store64" || name == "atomic_store64_relaxed" || name == "atomic_store_ptr" || name == "atomic_store_ptr_relaxed") {
        if (args.size() >= 2 && args[0].asPtr()) {
            *reinterpret_cast<uint64_t*>(args[0].asPtr()) = args[1].asInt();
        }
        return VMValue(0);
    }
    if (name == "atomic_fence") {
        return VMValue(0);
    }

    // Fallback: a symbol of the running process
    void* sym = processSymbol(name);
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

} // namespace eir
