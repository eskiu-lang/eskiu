// eir_asm_int.h: the ARM64 assembly emitter (Arm64Emitter, see eir_asm.h) and its
// helpers, shared by eir_asm_arm64.cpp and its parts eir_asm_*.cpp.
#ifndef EIR_ASM_INT_H
#define EIR_ASM_INT_H

#include "eir_asm.h"
#include "eir_runtime.h"
#include "../intrinsics.h"
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace eir {

// File-local helpers of the ARM64 emitter (their own namespace: the C emitter has helpers
// of the same names).
namespace a64emit {

inline bool isAggregate(const TypePtr& t) {
    return t && (t->isStruct() || t->isArray() || t->isClosure() || t->isInterface() || t->isSlice());
}

inline bool isVaListType(const TypePtr& t) {
    return t && t->isStruct() && t->name == "__va_list";
}

// An aggregate an extern cannot take or return by value (a fn-typed parameter of an extern
// is a C function pointer, and a va_list is passed as C passes one).
inline bool isCAggregate(const TypePtr& t) {
    return isAggregate(t) && !t->isClosure() && !isVaListType(t);
}

inline bool isRuntimeName(const std::string& n) {
    return n.rfind("__eir_", 0) == 0 || n == "__exception_text_into";
}

inline size_t alignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

// Where one argument travels: an integer or FP register, or the stack at an offset from
// the outgoing area (caller) or from the incoming area (callee).
struct ArgLoc {
    bool fp = false;
    int reg = -1;
    size_t stackOff = 0;
    size_t size = 8;
    bool vararg = false;
};

} // namespace a64emit

using namespace a64emit;

// Values live in 8-byte frame slots; an integer narrower than 64 bits is kept extended
// to 64 by its signedness, so comparisons and address arithmetic use the full register.
// An aggregate (struct, array, closure, interface, slice) is the address of its bytes:
// a Load of one yields the address, a Store copies the bytes. A function returning an
// aggregate writes it to the buffer its caller passes in x8 and returns that address.
class Arm64Emitter : public AsmEmitter {
public:
    explicit Arm64Emitter(const std::string& triple) : targetTriple(triple) {
        isDarwin = targetTriple.find("darwin") != std::string::npos ||
                   targetTriple.find("apple") != std::string::npos ||
                   targetTriple.find("macos") != std::string::npos ||
                   targetTriple.find("ios") != std::string::npos;
#if defined(__APPLE__)
        if (targetTriple.empty()) isDarwin = true;
#endif
    }

    void emitModule(const Module* mod, std::ostream& os) override;

private:
    std::string targetTriple;
    bool isDarwin = false;
    bool throws = false;
    const Module* currentMod = nullptr;
    std::unordered_map<std::string, std::string> stringTable;
    std::vector<std::pair<std::string, std::string>> stringOrder;
    std::unordered_set<std::string> definedSyms;
    int nextStrId = 0;
    int nextLocal = 0;

    // Per-function state
    const Function* curFn = nullptr;
    std::string fnTag;
    std::unordered_map<std::string, size_t> slots;
    std::unordered_set<std::string> addrVars;
    std::unordered_map<std::string, TypePtr> resultTypes;
    std::unordered_map<const Instruction*, size_t> callScratch;   // a C call's copies of its records
    // Functions handed to C that take or return a record, in the order the module first
    // takes their address (cabiOrder), so the thunks come out in a fixed order.
    std::unordered_set<std::string> cabiFns;
    std::vector<std::string> cabiOrder;
    std::unordered_map<std::string, int> handlerIds;
    size_t sretSlot = 0, handlerDepthSlot = 0, handlerStackSlot = 0, handlerCap = 0;
    size_t caughtSlot = 0, retSaveSlot = 0, incomingStackBytes = 0;
    size_t grSaveSlot = 0, vrSaveSlot = 0;
    int fixedGr = 0, fixedVr = 0;
    bool fnHasSret = false, fnSavesCaught = false;

    std::string localPrefix() const { return isDarwin ? "L" : ".L"; }
    std::string newLocal(const std::string& what) { return localPrefix() + what + "_" + std::to_string(nextLocal++); }

    static std::string escapeAsm(const std::string& s);

    // An `export` function whose signature has a record or a va_list: C calls the thunk under
    // the function's own name, and its body is `<name>.esk`.
    bool exportEntry(const Function* f) const;

    // The symbol Eskiu code calls a function by: an export entry's body, else its name.
    std::string bodySym(const std::string& name) const;

    // The C-ABI thunk of a function: the export entry's own name, else `__cabi_<name>`.
    std::string cabiSym(const std::string& name) const;

    // A symbol name: characters an assembler identifier cannot hold are spelled `$XX`.
    std::string mangleSym(const std::string& name) const;

    std::string blockLabel(const std::string& bb) const;

    void collectStrings(const Module* mod);

    size_t sizeOf(const TypePtr& t) const;

    static int64_t truncConst(int64_t v, const TypePtr& t);

    void emitGlobal(const GlobalVar& g, std::ostream& os);

    // ---- Immediates and frame addressing ----

    void movImm(const std::string& reg, int64_t v, std::ostream& os);

    void addImm(const std::string& rd, const std::string& rn, size_t imm, std::ostream& os);

    // `op reg, [base, #off]` with the scaled unsigned offset form when it encodes, else the
    // offset in x16.
    void memOp(const std::string& op, const std::string& reg, const std::string& base, size_t off,
               size_t scale, std::ostream& os);

    size_t slotOf(const std::string& v) const;

    // Store an instruction's result: an integer is first normalized to its type (every
    // integer value is held wrapped to its width and extended by its signedness).
    void storeX(const std::string& reg, const std::string& var, std::ostream& os);

    void storeFloat(int fr, bool is32, const std::string& var, std::ostream& os);

    // Extend the low bits of x<r> to 64 by the type's width and signedness.
    void normalize(int r, const TypePtr& t, std::ostream& os);

    void loadAddrOfSym(const std::string& reg, const std::string& name, std::ostream& os);

    void loadOperand(const Operand& op, int regIdx, std::ostream& os);

    // Load a value into s<r>/d<r> as the width `want32` asks, converting from the
    // operand's own type (an integer converts by its signedness).
    void loadFloat(const Operand& op, int r, bool want32, std::ostream& os);

    // ---- Calling convention ----

    size_t argSize(const TypePtr& t) const;

    std::vector<ArgLoc> classify(const std::vector<TypePtr>& tys, size_t fixedCount, bool variadic, size_t* stackBytes) const;

    struct CallShape {
        std::vector<TypePtr> argTypes;
        size_t fixedCount = 0;
        bool variadic = false;
        bool external = false;
        bool indirectEnv = false;
    };

    CallShape callShape(const Instruction* inst) const;

    static bool isSpecialCall(const std::string& n) {
        return n == "va_start" || n == "va_end" || n == "va_arg" || n == "llvm.va_start.p0" || n == "llvm.va_end.p0";
    }

    // ---- C ABI for records (AAPCS64) ----
    //
    // A struct or union passed by value to or from C: a homogeneous floating-point
    // aggregate (1-4 members of one float type) in consecutive v registers, else up to
    // 16 bytes in x registers, else a pointer to a copy; when the registers run out, the
    // record goes on the stack. A result: an HFA in v0-v3, up to 16 bytes in x0/x1, else in
    // the buffer x8 points to.

    // The scalar leaves of a type, in order (a union contributes its widest member's,
    // when all members agree; a bitfield an integer, which is never an HFA member).
    void leaves(const TypePtr& t, std::vector<TypePtr>& out, int depth = 0);

    // The HFA member count (0: not one) and whether its members are doubles.
    int hfaOf(const TypePtr& t, bool& isDouble);

    struct CLoc {
        enum Kind { Gpr, Fpr, Stack, Hfa, Words, StackCopy, Indirect } kind = Gpr;
        int reg = 0, count = 0;
        size_t off = 0, size = 8;
        bool dbl = false, vararg = false, viaStack = false;
    };

    std::vector<CLoc> classifyC(const std::vector<TypePtr>& tys, size_t fixedCount, bool variadic, size_t* stackBytes);

    bool isCRecordCall(const Instruction* inst, const CallShape& cs) const;

    // A call to C with records: each record is first copied to the call's scratch area
    // (so a register load never reads past the record), then placed by classifyC.
    void emitCRecordCall(const Instruction* inst, const CallShape& cs, std::ostream& os);

    bool hfaRet(const TypePtr& t);

    // A function whose address is handed to C (an operand other than a direct call's or a
    // closure's target) and whose signature has a record or a va_list.
    void findCabiFns(const Module* mod);

    // `__cabi_<f>`: the C signature, calling f with the Eskiu one (records by address, a
    // va_list in a slot of its own, a record result through f's buffer).
    void emitCabiThunk(const Function* fn, std::ostream& os);

    // ---- Functions ----

    void emitFunction(const Function* fn, int idx, std::ostream& os);

    // Load a value of type t from [base + off] into `reg` (x form), extended by its type.
    void loadSized(const std::string& xreg, const std::string& base, size_t off, const TypePtr& t, std::ostream& os);

    // Copy `size` bytes from [x9] to [x10] (clobbers x9-x13).
    void copyBytes(size_t size, std::ostream& os);

    void divCheck(const Operand& divisor, std::ostream& os);

    void exceptionCheck(std::ostream& os, const std::string& unwindLabel);

    // ---- Instructions ----

    void emitInstruction(const Instruction* inst, const std::string& retLabel,
                         const std::string& unwindLabel, std::ostream& os);
    void emitMemoryInst(const Instruction* inst, std::ostream& os, const std::vector<Operand>& ops);
    void emitArithInst(const Instruction* inst, std::ostream& os, const std::vector<Operand>& ops);
    void emitCompareConvInst(const Instruction* inst, std::ostream& os, const std::vector<Operand>& ops);
    void emitControlInst(const Instruction* inst, const std::string& retLabel, const std::string& unwindLabel, std::ostream& os, const std::vector<Operand>& ops);

    // ---- Calls ----

    void emitVaStart(const Operand& ap, std::ostream& os);

    // x9 <- address of the next argument of type t, advancing the va_list at [x10].
    void emitVaArgAddr(const TypePtr& t, std::ostream& os);

    bool emitSpecialCall(const Instruction* inst, std::ostream& os);

    // An `intrinsic` atomic, inline: acquire/release exclusives (plain ones when relaxed),
    // the cell's width by its kind.
    bool emitAtomic(const Instruction* inst, std::ostream& os);

    void emitCall(const Instruction* inst, const std::string& unwindLabel, std::ostream& os);

    void loadArgInt(const Operand& op, const TypePtr& t, bool external, int reg, std::ostream& os);

    // ---- Inline assembly ----

    void emitInlineAsm(const Instruction* inst, std::ostream& os);
};

} // namespace eir

#endif // EIR_ASM_INT_H
