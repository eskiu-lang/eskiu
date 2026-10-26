#pragma once

#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include <sstream>
#include <cstdint>
#include <utility>

namespace eir {

// ============================================================================
// Types
// ============================================================================

enum class TypeKind {
    Void,
    Int,
    Float,
    Pointer,
    Struct,
    Array,
    Function,
    Interface,
    Closure,
    Slice
};

struct Type;
using TypePtr = std::shared_ptr<Type>;

struct Type {
    TypeKind kind;
    std::string name;
    int bitWidth = 0;
    bool isSigned = true;

    TypePtr pointee;
    TypePtr elemType;
    uint64_t arraySize = 0;
    std::vector<TypePtr> members;
    TypePtr returnType;

    std::string str() const;

    bool isVoid() const { return kind == TypeKind::Void; }
    bool isInt() const { return kind == TypeKind::Int; }
    bool isFloat() const { return kind == TypeKind::Float; }
    bool isPointer() const { return kind == TypeKind::Pointer; }
    bool isStruct() const { return kind == TypeKind::Struct; }
    bool isArray() const { return kind == TypeKind::Array; }
    bool isSlice() const { return kind == TypeKind::Slice; }
    bool isClosure() const { return kind == TypeKind::Closure; }
    bool isInterface() const { return kind == TypeKind::Interface; }

    std::string nominalName() const {
        if (kind == TypeKind::Pointer && pointee) return pointee->nominalName();
        return name;
    }

    static TypePtr getVoid();
    static TypePtr getI1();
    static TypePtr getI8();
    static TypePtr getI16();
    static TypePtr getI32();
    static TypePtr getI64();
    static TypePtr getU8();
    static TypePtr getU16();
    static TypePtr getU32();
    static TypePtr getU64();
    static TypePtr getF32();
    static TypePtr getF64();
    static TypePtr getPtr(TypePtr pointee = nullptr);
    static TypePtr getStruct(const std::string& name, const std::vector<TypePtr>& fields);
    static TypePtr getArray(TypePtr elem, uint64_t size);
    static TypePtr getSlice(TypePtr elem);
    static TypePtr getInterface();
    static TypePtr getClosure();
    static TypePtr getFunction(TypePtr ret, const std::vector<TypePtr>& params);
};

// ============================================================================
// Operands
// ============================================================================

enum class OperandKind {
    Variable,
    ConstantInt,
    ConstantFloat,
    ConstantString,
    ConstantNull,
    GlobalRef,
    BlockRef
};

struct Operand {
    OperandKind kind;
    TypePtr type;
    std::string text;
    int64_t intVal = 0;
    double floatVal = 0.0;

    std::string str() const;

    static Operand var(const std::string& name, TypePtr type);
    static Operand constInt(int64_t val, TypePtr type);
    static Operand constFloat(double val, TypePtr type);
    static Operand constString(const std::string& val);
    static Operand constNull();
    static Operand global(const std::string& name, TypePtr type);
    static Operand block(const std::string& label);
};

// ============================================================================
// Instructions
// ============================================================================

enum class Opcode {
    // Memory
    Alloca,
    Load,
    Store,
    Gep,
    FieldPtr,
    OffsetPtr,

    // Arithmetic
    Add, Sub, Mul, SDiv, UDiv, SRem, URem,
    FAdd, FSub, FMul, FDiv,
    Neg,

    // Bitwise
    And, Or, Xor, Shl, LShr, AShr, Not,

    // Compare
    ICmp, FCmp,

    // Conversions
    Trunc, ZExt, SExt, FPTrunc, FPExt, FPToSI, SIToFP, Bitcast, PtrToInt, IntToPtr,

    // Control Flow / Terminators
    Br,
    BrCond,
    Switch,
    Ret,
    Unreachable,

    // Calls & Fat Pointers
    Call,
    CallIndirect,
    MakeInterface,
    MakeClosure,
    MakeSlice,
    ExtractFat,

    // Exceptions
    Throw,
    Invoke,
    LandingPad
};

enum class ICmpCond {
    Eq, Ne, Slt, Sle, Sgt, Sge, Ult, Ule, Ugt, Uge
};

enum class FCmpCond {
    Oeq, One, Olt, Ole, Ogt, Oge
};

std::string icmpCondToStr(ICmpCond cond);
std::string fcmpCondToStr(FCmpCond cond);

struct Instruction {
    Opcode op;
    std::string resultVar; // e.g. "%0", empty if void/terminator/store
    TypePtr type;          // Result type
    std::vector<Operand> operands;
    ICmpCond icmpCond = ICmpCond::Eq;
    FCmpCond fcmpCond = FCmpCond::Oeq;

    std::vector<std::pair<int64_t, std::string>> switchCases;
    std::string defaultLabel;

    Instruction() = default;
    Instruction(Opcode op, const std::string& res, TypePtr ty, const std::vector<Operand>& ops = {},
                ICmpCond icmp = ICmpCond::Eq, FCmpCond fcmp = FCmpCond::Oeq)
        : op(op), resultVar(res), type(ty), operands(ops), icmpCond(icmp), fcmpCond(fcmp) {}

    std::string str() const;
    bool isTerminator() const;
};

// ============================================================================
// BasicBlock
// ============================================================================

struct BasicBlock {
    std::string name;
    std::vector<std::unique_ptr<Instruction>> instructions;

    explicit BasicBlock(const std::string& name) : name(name) {}
    bool hasTerminator() const;
    void dump(std::ostream& os) const;
};

// ============================================================================
// Function
// ============================================================================

struct Function {
    std::string name;
    TypePtr returnType;
    std::vector<std::pair<std::string, TypePtr>> params;
    std::vector<std::unique_ptr<BasicBlock>> blocks;
    bool isExtern = false;
    bool isVariadic = false;

    BasicBlock* createBlock(const std::string& name = "");
    void dump(std::ostream& os) const;
};

// ============================================================================
// Module
// ============================================================================

struct GlobalVar {
    std::string name;
    TypePtr type;
    Operand init;
    bool isConst = false;
};

struct StructFieldInfo {
    std::string name;
    TypePtr type;
    size_t offset = 0;
    size_t size = 0;
};

struct StructInfo {
    std::string name;
    std::vector<StructFieldInfo> fields;
    size_t totalSize = 0;
    size_t alignment = 1;

    size_t getFieldOffset(const std::string& fieldName) const {
        for (const auto& f : fields) {
            if (f.name == fieldName) return f.offset;
        }
        return 0;
    }
};

size_t getTypeSize(TypePtr t);
size_t getTypeAlign(TypePtr t);

struct Module {
    std::string name;
    std::string targetTriple;
    std::vector<TypePtr> structTypes;
    std::unordered_map<std::string, StructInfo> structLayouts;
    std::vector<GlobalVar> globals;
    std::vector<std::unique_ptr<Function>> functions;

    Function* getFunction(const std::string& name) const;
    Function* createFunction(const std::string& name, TypePtr ret, const std::vector<std::pair<std::string, TypePtr>>& params);
    size_t getFieldOffset(const std::string& structName, const std::string& fieldName) const {
        auto it = structLayouts.find(structName);
        if (it != structLayouts.end()) return it->second.getFieldOffset(fieldName);
        return 0;
    }
    size_t getStructSize(const std::string& structName) const {
        auto it = structLayouts.find(structName);
        if (it != structLayouts.end()) return it->second.totalSize;
        return 0;
    }
    void dump(std::ostream& os) const;
    std::string str() const;
};

} // namespace eir
