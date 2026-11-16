// eir_c_int.h: the C99 emitter (CEmitterImpl, see eir_c.h) and its helpers, shared by
// eir_c.cpp and its parts eir_c_*.cpp.
#ifndef EIR_C_INT_H
#define EIR_C_INT_H

#include "eir_c.h"
#include "eir_runtime.h"
#include "../intrinsics.h"
#include <iostream>
#include <sstream>
#include <unordered_set>
#include <unordered_map>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <stdexcept>

namespace eir {

// File-local helpers of the C emitter (their own namespace: the ARM64 emitter has
// helpers of the same names).
namespace cemit {

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

// An asm constraint as C writes it: an explicit x86 register in LLVM's form (`{ax}`,
// `{eax}`, `{rax}`, also after `=`, `&` or `+`) becomes its GCC letter (a b c d S D), which
// is what C accepts; anything else is kept.
inline std::string gccConstraint(const std::string& c) {
    size_t open = c.find('{');
    size_t close = c.find('}');
    if (open == std::string::npos || close == std::string::npos || close < open) return c;
    std::string reg = c.substr(open + 1, close - open - 1);
    if (reg.size() == 3 && (reg[0] == 'e' || reg[0] == 'r')) reg = reg.substr(1);
    static const char* const regs[][2] = {{"ax", "a"}, {"bx", "b"}, {"cx", "c"}, {"dx", "d"}, {"si", "S"}, {"di", "D"}};
    for (const auto& r : regs)
        if (reg == r[0]) return c.substr(0, open) + r[1] + c.substr(close + 1);
    return c;
}

inline bool isRuntimeName(const std::string& n) {
    return n.rfind("__eir_", 0) == 0 || n == "__exception_text_into";
}

inline bool isVaCall(const std::string& n) {
    return n == "va_start" || n == "va_end" || n == "va_arg" || n == "llvm.va_start.p0" || n == "llvm.va_end.p0";
}

inline const std::unordered_set<std::string>& cKeywords() {
    static const std::unordered_set<std::string> kw = {
        "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else",
        "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long", "register",
        "restrict", "return", "short", "signed", "sizeof", "static", "struct", "switch",
        "typedef", "union", "unsigned", "void", "volatile", "while", "_Bool", "_Complex",
        "_Imaginary", "asm", "bool", "true", "false", "NULL"};
    return kw;
}

// The helpers every generated file carries (eir_c.cpp).
extern const char* kPrelude;

} // namespace cemit

using namespace cemit;

// Values follow the EVM: an integer is held in an int64_t and computed in 64 bits (a
// 32-bit add/sub/mul/sdiv wraps to 32, a narrow unsigned bitwise result is masked, a shift
// works at its type's width); memory reads extend by the type and writes truncate. An
// aggregate is the address of its bytes; a function returning one writes it to the buffer
// its caller passes first and returns that address. Function boundaries use exact C types.
class CEmitterImpl : public CEmitter {
public:
    void emitModule(const Module* mod, std::ostream& os) override;

private:
    const Module* currentMod = nullptr;
    std::unordered_map<std::string, std::string> nameCache;
    bool throws = false;

    // Per-function state
    const Function* curFn = nullptr;
    bool fnHasSret = false;
    bool fnSavesCaught = false;
    std::unordered_map<std::string, int> handlerIds;
    std::unordered_map<std::string, char> varKind;   // 'i' int64_t, 'f' float, 'd' double, 'p' void*
    size_t handlerCap = 0;
    int nextTmp = 0;

    // ---- Names ----

    bool isDefined(const std::string& n) const;

    bool isGlobal(const std::string& n) const;

    // An Eskiu symbol as a C identifier: other characters are spelled `_xHH`, and a name
    // C reserves is prefixed. An extern keeps its name: it is the C symbol.
    std::string cName(const std::string& n);

    // An `export` function whose signature has a record or a va_list: C calls the thunk
    // under the function's own name, and its body is `__eskc_body_<name>`.
    bool exportEntry(const Function* f) const {
        return f && f->isExport && !f->isExtern && !f->blocks.empty() && cabiSignature(f);
    }

    static bool cabiSignature(const Function* f);

    // The C name of an export entry's thunk: the function's name, spelled as cName does.
    std::string exportName(const std::string& n);

    static std::string sanitize(const std::string& prefix, const std::string& v);
    static std::string var(const std::string& v) { return sanitize("v_", v); }
    static std::string label(const std::string& l) { return sanitize("bb_", l); }

    // ---- Types and constants ----

    // The exact C type, for memory, function boundaries and externs.
    static std::string cType(const TypePtr& t);

    static char kindOf(const TypePtr& t);

    static std::string kindType(char k) {
        return k == 'i' ? "int64_t" : k == 'f' ? "float" : k == 'd' ? "double" : "void*";
    }

    static int64_t truncConst(int64_t v, const TypePtr& t);

    static std::string i64Const(int64_t v) {
        return "((int64_t)UINT64_C(" + std::to_string(static_cast<uint64_t>(v)) + "))";
    }

    static std::string floatConst(double v, bool is32);

    static std::string escape(const std::string& str);

    size_t sizeOf(const TypePtr& t) const;

    std::string symAddr(const std::string& n);

    char opKind(const Operand& op) const;

    // The operand as an int64_t (a pointer through intptr_t, a float converted).
    std::string ival(const Operand& op);
    std::string uval(const Operand& op) { return "((uint64_t)" + ival(op) + ")"; }

    // The operand as a void*.
    std::string pval(const Operand& op);

    // The operand as a double (an integer converted by its signedness).
    std::string fval(const Operand& op);

    // The operand in the representation of a variable of kind k.
    std::string asKind(const Operand& op, char k);

    // The operand converted to the exact C type of t.
    std::string asC(const Operand& op, const TypePtr& t);

    // ---- Declarations ----

    void emitPrototype(const Function* fn, std::ostream& os);

    // Symbols the program reaches without declaring them: a C function the builder calls
    // by name (strcmp in a catch dispatch) gets an unprototyped declaration, data (a C
    // global such as stderr) an incomplete array, so its name is its address.
    void emitImplicitDecls(const Module* mod, std::ostream& os);

    // ---- C records (structs and unions crossing an extern) ----

    std::unordered_map<std::string, std::string> recordNames;
    std::ostringstream recordDefs;
    // Functions handed to C that take/return a record, in the order the module first takes
    // their address (cabiOrder), so the records and thunks come out in a fixed order.
    std::unordered_set<std::string> cabiFns;
    std::vector<std::string> cabiOrder;

    // A member declaration `T name` (an array's dimensions after the name).
    std::string memberDecl(const TypePtr& t, const std::string& name);

    static std::string scalarField(const TypePtr& t) {
        return t && t->isInt() && t->bitWidth == 1 ? "_Bool" : cType(t);
    }

    // The C spelling of a struct or union, defined on first use (its members first).
    std::string cRecord(const TypePtr& t);

    // A function handed to C by address whose signature has a record is reached through a
    // thunk with the C signature that calls it with the Eskiu one.
    void findCabiFns(const Module* mod);

    // The C-ABI thunks to emit: those of the functions handed to C, then the export
    // entries' (in module order).
    std::vector<std::string> thunkOrder() const;

    void emitCabiThunk(const Function* fn, std::ostream& os);

    std::string globalTypeName(size_t i) { return "__eskc_g" + std::to_string(i) + "_t"; }
    std::vector<std::vector<std::pair<std::string, std::string>>> globalInits;

    // A global is a union of its bytes and a packed struct of its initializer's elements.
    void emitGlobalDecls(const Module* mod, std::ostream& os);

    void emitGlobalDefs(const Module* mod, std::ostream& os);

    // ---- Functions ----

    void emitFunction(const Function* fn, std::ostream& os);

    // Return the operand (null: the zero value) from the current function.
    void emitReturn(const Operand* val, std::ostream& os);

    // A read of type t at addr, in the representation of its variable.
    std::string loadExpr(const TypePtr& t, const std::string& addr);

    void exceptionCheck(std::ostream& os) {
        if (throws) os << "    if (__eir_exc_pending()) goto __eskc_unwind;\n";
    }

    // A narrow integer result in its type's 64-bit form.
    static std::string narrow(const std::string& e, const TypePtr& t);

    // ---- Instructions ----

    // `res = e;` with e an int64_t, converted to the variable's representation (an EIR
    // value typed as a pointer can come out of integer arithmetic).
    std::string setInt(const Instruction* inst, const std::string& e);

    void emitInstruction(const Instruction* inst, std::ostream& os);

    void emitInstructionBody(const Instruction* inst, std::ostream& os);
    void emitMemoryInst(const Instruction* inst, std::ostream& os, const std::vector<Operand>& ops, const std::string& res, const TypePtr& rt);
    void emitArithInst(const Instruction* inst, std::ostream& os, const std::vector<Operand>& ops, const std::string& res, const TypePtr& rt);
    void emitCompareConvInst(const Instruction* inst, std::ostream& os, const std::vector<Operand>& ops, const std::string& res, const TypePtr& rt);
    void emitControlInst(const Instruction* inst, std::ostream& os, const std::vector<Operand>& ops, const std::string& res, const TypePtr& rt);

    std::string lastParamName() const;

    // Store a call's result (an expression of C type t) into its variable.
    std::string fromC(const std::string& e, const TypePtr& t);

    void emitCall(const Instruction* inst, std::ostream& os);

    // An `intrinsic` atomic through the GNU __atomic builtins, with the orderings of
    // intrinsics.h.
    void emitAtomic(const Instruction* inst, const AtomicIntrinsic& a, std::ostream& os);

    // GNU extended asm from the LLVM-style operands the builder records ($N, *m for
    // memory, digits for ties).
    void emitInlineAsm(const Instruction* inst, std::ostream& os);
};

} // namespace eir

#endif // EIR_C_INT_H
