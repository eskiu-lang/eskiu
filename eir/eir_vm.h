#pragma once

#include "eir.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <memory>
#include <cstdint>
#include <cstring>

namespace eir {

// ============================================================================
// VM Value Representation
// ============================================================================

struct VMValue {
    enum class Kind { Int, Float, Ptr } kind = Kind::Int;
    union {
        int64_t i;
        double f;
        void* p;
    };

    VMValue() : kind(Kind::Int), i(0) {}
    explicit VMValue(int val) : kind(Kind::Int), i(val) {}
    explicit VMValue(int64_t val) : kind(Kind::Int), i(val) {}
    explicit VMValue(uint64_t val) : kind(Kind::Int), i(static_cast<int64_t>(val)) {}
    explicit VMValue(double val) : kind(Kind::Float), f(val) {}
    explicit VMValue(void* ptr) : kind(Kind::Ptr), p(ptr) {}
    explicit VMValue(std::nullptr_t) : kind(Kind::Ptr), p(nullptr) {}

    int64_t asInt() const {
        if (kind == Kind::Float) return static_cast<int64_t>(f);
        if (kind == Kind::Ptr) return static_cast<int64_t>(reinterpret_cast<uintptr_t>(p));
        return i;
    }

    double asFloat() const {
        if (kind == Kind::Float) return f;
        return static_cast<double>(i);
    }

    void* asPtr() const {
        if (kind == Kind::Ptr) return p;
        return reinterpret_cast<void*>(static_cast<uintptr_t>(i));
    }

    bool asBool() const {
        return asInt() != 0;
    }
};

// ============================================================================
// Stack Frame
// ============================================================================

struct StackFrame {
    Function* function = nullptr;
    std::unordered_map<std::string, VMValue> registers;
    std::vector<uint8_t> stackMemory;
    size_t stackOffset = 0;

    void* allocStack(size_t bytes, size_t align = 8) {
        if (align < 1) align = 1;
        size_t aligned = (stackOffset + align - 1) & ~(align - 1);
        stackOffset = aligned + bytes;
        if (stackOffset > stackMemory.size()) {
            stackMemory.resize(std::max(stackMemory.size() * 2, stackOffset + 4096));
        }
        std::memset(&stackMemory[aligned], 0, bytes);
        return &stackMemory[aligned];
    }
};

// ============================================================================
// EIR Virtual Machine
// ============================================================================

class VM {
public:
    VM();
    ~VM() = default;

    // Run the specified entry function in the given module
    int run(Module* module, const std::string& entryFn = "main", const std::vector<std::string>& args = {});

private:
    Module* currentModule = nullptr;

    // Global memory storage
    std::unordered_map<std::string, std::vector<uint8_t>> globalStorage;
    std::unordered_map<std::string, std::string> internedStrings;

    // Execution
    VMValue executeFunction(Function* fn, const std::vector<VMValue>& args);
    VMValue getOperand(const Operand& op, StackFrame& frame);
    void writeMemory(void* ptr, TypePtr type, const VMValue& val);
    VMValue readMemory(void* ptr, TypePtr type);

    // External C function calls
    VMValue callExtern(const std::string& name, const std::vector<VMValue>& args, TypePtr retTy);
    void handlePrintf(const std::string& format, const std::vector<VMValue>& args);
};

} // namespace eir
