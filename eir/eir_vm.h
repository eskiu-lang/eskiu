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
    std::vector<VMValue> varargs;
    std::vector<BasicBlock*> exceptionHandlers;
    std::vector<std::unique_ptr<uint8_t[]>> chunks;
    size_t currentChunkOffset = 0;
    static constexpr size_t CHUNK_SIZE = 64 * 1024;

    void* allocStack(size_t bytes, size_t align = 8) {
        if (align < 1) align = 1;
        if (chunks.empty()) {
            chunks.push_back(std::make_unique<uint8_t[]>(CHUNK_SIZE));
            currentChunkOffset = 0;
        }

        size_t aligned = (currentChunkOffset + align - 1) & ~(align - 1);
        if (aligned + bytes <= CHUNK_SIZE) {
            currentChunkOffset = aligned + bytes;
            void* ptr = &chunks.back()[aligned];
            std::memset(ptr, 0, bytes);
            return ptr;
        }

        size_t allocSize = std::max(CHUNK_SIZE, bytes + align);
        chunks.push_back(std::make_unique<uint8_t[]>(allocSize));
        currentChunkOffset = bytes;
        void* ptr = &chunks.back()[0];
        std::memset(ptr, 0, bytes);
        return ptr;
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
    std::string formatString(const std::string& format, const std::vector<VMValue>& args);
    void handlePrintf(const std::string& format, const std::vector<VMValue>& args);

    // Buffer for returning aggregates / fat pointers across stack frames
    std::vector<uint8_t> returnBuffer;

    // Active va_list state for variadics and vsnprintf
    struct VMVaList {
        std::vector<VMValue> args;
        size_t index = 0;
    };
    std::unordered_map<void*, VMVaList> activeVaLists;

    // Exception handling state
    void* currentException = nullptr;
    std::vector<void*> caughtExceptionStack;
    std::string activeExceptionText;

    bool hasException() const { return currentException != nullptr; }
    void clearException() { currentException = nullptr; }
    std::string formatException(void* exPtr);
};

} // namespace eir
