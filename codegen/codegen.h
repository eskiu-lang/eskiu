#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>
#include <stack>
#include "../ast/ast.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Value.h"

// True when `bb` already ends in a terminator. LLVM 23 made
// BasicBlock::getTerminator() assert on an unterminated block instead of
// returning null, so every "is this block closed?" test goes through here.
inline bool hasTerminator(const llvm::BasicBlock* bb) {
    return !bb->empty() && bb->back().isTerminator();
}

// An integer constant of type `ty` holding the low bits of `v`. LLVM 23 stopped
// implicitly truncating ConstantInt::get(ty, uint64_t) to the type's width, so a
// sign-extended or wider value (a negative literal, `~x`, a char >= 0x80) must be
// masked to the width first; this is the same bit pattern on every LLVM version.
inline llvm::ConstantInt* constIntBits(llvm::Type* ty, uint64_t v) {
    unsigned bits = ty->getIntegerBitWidth();
    if (bits < 64) v &= (uint64_t(1) << bits) - 1;
    return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(ty), v);
}

namespace llvm { class Triple; }
// Does an unnamed bitfield (`T : N` or `T : 0`) raise the alignment of its struct or
// union to T's, as on AAPCS (32-bit ARM and AArch64, outside Darwin and Windows)? Elsewhere
// clang's Itanium layout ignores it for alignment.
bool unnamedBitfieldsAlign(const llvm::Triple& t);

class CodeGen : public ASTVisitor {
public:
    CodeGen();
    ~CodeGen();

    // Generate code — fills internal module, returns raw pointer (null on failure)
    llvm::Module* generateCode(std::shared_ptr<Program> program);

    // Get the generated LLVM module (non-owning)
    llvm::Module* getModule() const { return module.get(); }

    // Optional target triple override (empty = native)
    std::string targetTriple;
    // Optional target CPU override (empty = the target baseline: "apple-m1" on arm64 Apple, else "generic").
    // e.g. "mpcore" for the 3DS ARM11 (armv6k + VFPv2).
    std::string targetCPU;
    // Optional target feature string (LLVM -mattr syntax, e.g. "+vfp2").
    std::string targetFeatures;
    // Relocation model: "pic" (default), "static", or "dynamic-no-pic".
    // The 3DS .3dsx loader applies static relocations and has no dynamic loader to
    // populate a GOT, so 3dsx targets must use "static".
    std::string relocModel;
    // --freestanding: no libc, so the optimizer may not introduce calls to memset,
    // memcpy and the like (clang -ffreestanding).
    bool freestanding = false;
    // Safe mode (--safe[=LIST]): the run-time checks to insert, a set of the Safe*
    // bits (array/slice bounds, signed + - * overflow, null dereference). Off by
    // default, so release builds carry no overhead. A violation calls @llvm.trap.
    enum : unsigned { SafeBounds = 1, SafeOverflow = 2, SafeNull = 4, SafeAll = 7 };
    unsigned safeChecks = 0;
    // Emit a trap (via @llvm.trap) when `idx` (sext to i64) is < 0 or >= `len`.
    void emitBoundsCheck(llvm::Value* idx, llvm::Value* len);
    // --safe check for a slice construction `base[lo..hi]`: 0 <= lo <= hi <= len. A null
    // len (a raw-pointer base, whose length is unknown here) checks only 0 <= lo <= hi.
    void emitSliceBoundsCheck(llvm::Value* lo, llvm::Value* hi, llvm::Value* len);
    // Sanitizer instrumentation, applied to the module before object emission.
    bool asan = false;   // AddressSanitizer (memory errors); needs the asan runtime
    bool ubsan = false;  // bounds checking; traps on out-of-bounds (no runtime)
    // Optimization level (0 = -O0, no middle-end; 1..3 run the LLVM optimizer).
    unsigned optLevel = 0;
    // -ffp-contract=fast: every floating-point operation carries the `contract` flag, so
    // the backend may fuse a multiply and an add (FMA). Off: results are the same on every
    // target.
    bool fpContract = false;

    // Single-resolver table: post-AsyncTransform type checker's expressionTypeMap.
    // When set, getExprEskiuType returns these resolved types instead of re-deriving.
    const std::map<Expr*, std::string>* resolvedExprTypes = nullptr;
    // The type checker's generic instances (mangled -> template + args): an instance
    // reached only through a resolved expression type (a call result) is built on demand.
    const std::map<std::string, std::pair<std::string, std::vector<std::string>>>* semaInstanceArgs = nullptr;

    // Print LLVM IR to stdout
    void printIR() const;

    // Run the LLVM middle-end optimization pipeline at `optLevel` (1..3) over the
    // module, in place. No-op at level 0. Call after generateCode, before printing
    // or emitting. (-O0 keeps today's naive-IR-straight-to-backend behavior.)
    void optimizeModule();

    // Emit native object file
    bool emitObjectFile(const std::string& filename);

    // `eskiuc repl`: how a value of an Eskiu type is laid out in memory, so the REPL can
    // print it after the JIT ran the input (repl.cpp). Built from the module's types while
    // the module is still here.
    struct ReplLayout {
        enum Kind { Int, UInt, Bool, Char, Float, Double, CStr, Ptr, Struct, Enum, Variant,
                    List, Str, Array, Slice, Opaque };
        struct Field { std::string name; uint64_t offset = 0; std::shared_ptr<ReplLayout> layout; };
        struct Alt { std::string name; std::vector<Field> payload; };
        Kind kind = Opaque;
        std::string name;                      // the type as written (for Opaque)
        unsigned size = 0;                     // bytes of a scalar
        std::vector<Field> fields;             // Struct
        std::map<long long, std::string> enumNames;   // Enum
        std::map<long long, Alt> alts;         // Variant, by tag
        uint64_t dataOff = 0, lenOff = 0;      // List / Str / Slice
        unsigned lenSize = 0;
        uint64_t count = 0;                    // Array
        uint64_t stride = 0;                   // List / Array / Slice element size
        std::shared_ptr<ReplLayout> elem;      // List / Array / Slice element
    };
    std::shared_ptr<ReplLayout> replLayout(const std::string& type, int depth = 0);
    // Hand the module and its context over (to the JIT). The CodeGen is spent afterwards.
    std::unique_ptr<llvm::Module> takeModule(std::unique_ptr<llvm::LLVMContext>& ctx);

private:
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::IRBuilder<>> builder;

    // Symbol table: maps variable/function names to LLVM Values
    std::map<std::string, llvm::Value*> symbolTable;
    std::vector<std::map<std::string, llvm::Value*>> scopeStack;

    // Current function being compiled
    llvm::Function* currentFunction = nullptr;

    // Concrete struct registry
    std::map<std::string, llvm::StructType*> structTypes;
    std::map<std::string, std::vector<StructDecl::Field>> structFields;
    // The index of the first field named `member` in `fields` (struct `sname`'s field
    // list), or -1: a hash index per struct, rebuilt when the list changes, so a member
    // access does not scan every field.
    struct FieldIndex { const void* data = nullptr; size_t size = 0; std::unordered_map<std::string, size_t> idx; };
    std::map<std::string, FieldIndex> fieldIndexes;
    long fieldIndexOf(const std::string& sname, const std::vector<StructDecl::Field>& fields, const std::string& member) {
        FieldIndex& fx = fieldIndexes[sname];
        if (fx.data != (const void*)fields.data() || fx.size != fields.size()) {
            fx.idx.clear();
            for (size_t i = 0; i < fields.size(); ++i) fx.idx.emplace(fields[i].name, i);
            fx.data = fields.data();
            fx.size = fields.size();
        }
        auto it = fx.idx.find(member);
        return it == fx.idx.end() ? -1 : (long)it->second;
    }

    // Bitfield layout: for structs that contain at least one bitfield, every
    // field maps to a physical slot in the packed LLVM struct.
    struct BitfieldSlot {
        bool isBitfield = false;
        unsigned physIndex = 0;     // index into the physical LLVM struct
        unsigned bitOffset = 0;     // bit position within the storage word
        unsigned bitWidth  = 0;     // bitfield width
        llvm::Type* storageType = nullptr;  // physical slot type (a bitfield's declared type)
        bool isSigned = false;
        // C layout (non-MS targets): the field is addressed by byte offset, not by a
        // struct element; a bitfield is read through `accessType` (its declared type's
        // storage unit, or the exact byte span in a packed struct) at `accessAlign`.
        bool byOffset = false;
        uint64_t byteOffset = 0;
        llvm::Type* accessType = nullptr;
        unsigned accessAlign = 0;
    };
    // Lay out a struct with bitfields like C on the target: the MS rules on Windows (a
    // new storage unit when the declared type size changes), else the SysV/AAPCS rules
    // (a bitfield shares the current unit of its declared type if it fits). Fills the
    // physical element types and the per-field slots; `llvmPacked` = emit `<{ }>`.
    // `fields` is layoutFields(): an unnamed bitfield takes bits but gets no slot.
    void layoutBitfieldStruct(const std::vector<StructDecl::Field>& fields, bool packed,
                              unsigned packN, std::vector<llvm::Type*>& phys,
                              std::map<std::string, BitfieldSlot>& slots, bool& llvmPacked,
                              uint64_t& cAlign,
                              std::vector<std::pair<uint64_t, uint64_t>>& unnamedData);
    // Unnamed bitfields of a struct or union type, for the C ABI lowering: the byte ranges
    // each counts as data for the x86-64 register widths (clang's BitsContainNoUserData:
    // from its first bit through its declared type's size, within the type; a type with an
    // unnamed bitfield has an entry), and the types holding one of nonzero width (directly
    // or in a field), which are never a homogeneous FP aggregate.
    std::map<llvm::StructType*, std::vector<std::pair<uint64_t, uint64_t>>> unnamedBitData;
    std::set<llvm::StructType*> notHomogeneous;
    // The C alignment of a struct type LLVM lays out as packed (explicit padding) though
    // C aligns it: a `#pragma pack(N>=2)` struct (min(N, largest field alignment)), a
    // struct or union holding one, a bitfield struct laid out by hand. Only entries
    // above 1.
    std::map<llvm::StructType*, uint64_t> cAlignOverride;
    int recvTmpCount = 0;   // hidden locals holding a generic dot-call's receiver address
    // The C alignment of `t`: its cAlignOverride (an array's element's), else LLVM's.
    uint64_t cAlignOf(llvm::Type* t) const;
    // Address of a field of a bitfield-layout struct `sname` at `base`.
    llvm::Value* layoutFieldAddr(const std::string& sname, llvm::Value* base,
                                 const BitfieldSlot& slot, const llvm::Twine& name = "");
    std::map<std::string, std::map<std::string, BitfieldSlot>> structLayout;
    std::string structBaseTypeOf(const ExprPtr& base);  // resolve a member base to a struct name
    // Normalize a resolved type string to its bare struct/registry key: drop the
    // `struct:` tag and pointer decoration, and mangle+instantiate a template type.
    std::string stripToStructKey(std::string t);
    void storeBitfield(MemberExpr* m, llvm::Value* val); // read-modify-write a bitfield
    // Storage-word address of bitfield member `m` (base evaluated once); sets `slot`.
    llvm::Value* bitfieldWordPtr(MemberExpr* m, const BitfieldSlot*& slot);
    // Extract (shift, mask, sign-extend) a bitfield's value from its storage word.
    llvm::Value* loadBitfieldFrom(llvm::Value* wordPtr, const BitfieldSlot& slot, bool vol = false);
    // Masked read-modify-write of a bitfield given the storage-word pointer.
    void storeBitfieldInto(llvm::Value* wordPtr, const BitfieldSlot& slot, llvm::Value* val,
                           bool unsignedSrc = false, bool vol = false);

    // Template struct registry
    std::map<std::string, StructDecl*> templateDecls;
    // Reverse map: mangled instance name -> (template name, concrete type args).
    // Lets us recover that `List_int` is `List` instantiated with [int] when
    // inferring a type parameter from a `List<T>*` argument.
    std::map<std::string, std::pair<std::string, std::vector<std::string>>> templateInstanceArgs;
    // Structural unification: bind type params in `pattern` (e.g. List<T>*) from
    // a concrete argument type (e.g. *List_int), filling `subs`.
    void unifyTypeParam(std::string pattern, std::string concrete,
                        const std::set<std::string>& tps,
                        std::map<std::string, std::string>& subs) const;

    // Interface registry: name → vtable type + method order
    std::map<std::string, llvm::StructType*> ifaceVtableTypes;
    std::map<std::string, std::vector<std::string>> ifaceMethodOrder;
    // Fat pointer type per interface: %I = type { ptr, ptr }
    std::map<std::string, llvm::StructType*> ifaceFatPtrTypes;

    // Eskiu param types per function — for interface boxing at call sites
    std::map<std::string, std::vector<std::string>> funcEskiuParamTypes;
    // Eskiu return type per function — lets getExprEskiuType resolve the static
    // type of a call result (so member access on a temporary works).
    std::map<std::string, std::string> funcEskiuReturnType;

    // Interface method return types — indexed by [ifaceName][methodIndex]
    std::map<std::string, std::vector<std::string>> ifaceMethodReturnTypes;
    // Interface method param Eskiu types (excluding self) — [ifaceName][methodIndex]
    std::map<std::string, std::vector<std::vector<std::string>>> ifaceMethodParamEskiuTypes;

    // Global-scope variable type tracking (complement to varTypeStack which is function-scoped)
    std::map<std::string, std::string> globalVarTypes;

    // Evaluate an expression as an LLVM Constant (for global variable initializers).
    // Returns nullptr for expressions that cannot be folded to a constant.
    llvm::Constant* evaluateConstantExpr(const ExprPtr& expr);
    // Fold an initializer to a constant of `declType`, handling array literals
    // (`{...}`) element-wise with C-style zero-fill. Falls back to scalar folding +
    // coercion. Returns nullptr when the initializer isn't a compile-time constant.
    llvm::Constant* constInitializer(const ExprPtr& expr, llvm::Type* declType);
    // A bitfield-struct or union literal folded through its byte image (nullptr if a
    // member is not constant or has no byte form where the layout needs one).
    llvm::Constant* constAggregateImage(StructInitExpr* si, const std::string& sname);

    // Helpers
    llvm::Value* boxAsInterface(const std::string& ifaceName,
                                const std::string& structName,
                                llvm::Value* structPtr);
    // The interface named by `type` ("" if it is not an interface type).
    std::string interfaceName(const std::string& type) const;
    // Evaluate `e` for a slot of Eskiu type `targetType`: boxes a struct pointer when the
    // target is an interface, else a plain evaluateExpr.
    llvm::Value* evalForType(const ExprPtr& e, const std::string& targetType);
    // Wrap a top-level function in a {fn_ptr, env_ptr} closure value so a bare
    // function name can be passed where a fn(...)->R is expected. The synthesized
    // thunk ignores env and forwards to the target; cached per function.
    llvm::Value* makeFunctionPointer(llvm::Function* target);
    // The start routine for a thread that owns its closure (see visit(ThreadCreateExpr)).
    llvm::Function* ownedThreadTrampoline();
    void ensureTemplateInstantiated(const std::string& mangledName,
                                    const std::string& templateName,
                                    const std::vector<std::string>& args);
    // `t` with every template instance in it mangled to its struct name, keeping the
    // pointer / array / slice structure around it (`Box<int>*[4]` -> `Box_int*[4]`);
    // each instance is instantiated so its fields resolve.
    std::string instanceSpelling(const std::string& t);
    // Emit (once) the instance `mangledName` of the generic function `fd` under `subs`.
    llvm::Function* instantiateFnTemplate(FunctionDecl* fd, const std::string& mangledName,
                                          const std::map<std::string, std::string>& subs);
    // An inline method of a generic struct, for one instance (`Box_int` + `get` ->
    // `Box_int_get(*Box_int self)`): the template method (and its substitutions), or null.
    FunctionDecl* genericMethod(const std::string& instName, const std::string& method,
                                std::map<std::string, std::string>* subsOut) const;
    // A generic FREE function `S_m<T..>` taking an instance of the generic struct S first
    // (the `Type_method` convention, e.g. `List_push<T>(List<T>* self, T item)`), for
    // `x.m(...)` with x an instance of S: the template (null when there is none).
    FunctionDecl* genericFreeMethod(const std::string& instName, const std::string& method) const;
    // Its type arguments for receiver type `recvType` and the call's arguments.
    std::map<std::string, std::string> genericFreeMethodSubs(FunctionDecl* fd, const std::string& recvType,
                                                             const std::vector<ExprPtr>& args) const;
    // Emit that instance method on first use; returns it (null when there is none).
    llvm::Function* instantiateGenericMethod(const std::string& instName, const std::string& method);
    // Template function registry
    std::map<std::string, FunctionDecl*> funcTemplateDecls;
    // Active type param substitutions during template function instantiation
    std::map<std::string, std::string> typeParamOverride;

    // Volatile variable tracking — names of variables declared volatile
    std::set<std::string> volatileVars;
    bool volatileRooted(const Expr* e) const;
    llvm::LoadInst* volLoad(llvm::LoadInst* ld, const Expr* root) const;

    // Variable type tracking for MemberExpr/IndexExpr resolution
    std::vector<std::map<std::string, std::string>> varTypeStack;
    void defineVarType(const std::string& name, const std::string& type);
    std::string lookupVarType(const std::string& name) const;

    // Break/continue targets for the innermost loop
    llvm::BasicBlock* breakTarget    = nullptr;
    llvm::BasicBlock* continueTarget = nullptr;

    // Scope-exit cleanup stack: one frame per active block/try scope, holding the
    // `defer`/`errdefer` bodies (and a try's `finally`) to run LIFO when the scope is
    // left. Normal control-flow exits (return / break / continue / `?`) emit the frames
    // they leave before branching; block fall-through runs its own frame.
    // isErr = errdefer (error-path only). The body is emitted later, at an exit, where a
    // shadowing declaration may have rebound a name; `names`/`types` are the bindings
    // visible where it was registered, which the body is resolved against.
    // prevUnwind is the landingpad that was active before the cleanup was registered: its
    // body runs under it (an exception inside a defer body skips that defer), and popping
    // the frame that holds it restores it.
    struct Cleanup {
        Stmt* body; bool isErr;
        std::shared_ptr<const std::map<std::string, llvm::Value*>> names;
        std::shared_ptr<const std::vector<std::map<std::string, std::string>>> types;
        llvm::BasicBlock* prevUnwind = nullptr;
    };
    Cleanup makeCleanup(Stmt* body, bool isErr);
    std::vector<std::vector<Cleanup>> cleanupScopes;
    void popCleanupFrame();

    // Defers on the exceptional path. In a program that throws, each `defer` gets a
    // landingpad for the calls after it: inside a `try` body it runs the pending defers
    // down to that try and joins its catch dispatch (with the exception pointer); outside
    // any try it runs the function's pending defers and resumes unwinding.
    bool programUsesEH = false;
    struct TryCtx {
        size_t depth;                      // cleanup frame depth of the try body
        llvm::BasicBlock* dispatch;        // catch dispatch, entered with the exception ptr
        std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> incoming;
    };
    std::vector<TryCtx> tryStack;
    void emitDeferPad();
    bool enumBitfieldUnsigned(const std::string& type);
    void ensureEHRuntime();
    void emitUncaughtReporter();
    size_t breakCleanupDepth    = 0;   // frame depth to unwind to on break
    size_t continueCleanupDepth = 0;   // frame depth to unwind to on continue

    // Loops-only frame stack for labeled break/continue. Unlike breakTarget above (which a
    // `switch` also installs), this holds only real loops, so `break label` / `continue label`
    // can scan it top-down for the named loop and unwind to that loop's cleanup depth. Both
    // exits unwind to the same depth (the frame count at loop entry, before the body frame).
    struct LoopFrame {
        std::string label;                 // "" = unlabeled
        llvm::BasicBlock* breakBlock;
        llvm::BasicBlock* continueBlock;
        size_t cleanupDepth;
    };
    std::vector<LoopFrame> loopStack;

    // RAII: install a loop's break/continue targets and cleanup-unwind depth for the
    // duration of its body, restoring the enclosing loop's values on scope exit.
    struct LoopContext {
        CodeGen* cg;
        llvm::BasicBlock* pb; llvm::BasicBlock* pc; size_t pbd, pcd;
        LoopContext(CodeGen* c, llvm::BasicBlock* brk, llvm::BasicBlock* cont,
                    const std::string& label = "")
            : cg(c), pb(c->breakTarget), pc(c->continueTarget),
              pbd(c->breakCleanupDepth), pcd(c->continueCleanupDepth) {
            cg->breakTarget = brk; cg->continueTarget = cont;
            cg->breakCleanupDepth = cg->continueCleanupDepth = cg->cleanupScopes.size();
            cg->loopStack.push_back({label, brk, cont, cg->cleanupScopes.size()});
        }
        ~LoopContext() {
            cg->breakTarget = pb; cg->continueTarget = pc;
            cg->breakCleanupDepth = pbd; cg->continueCleanupDepth = pcd;
            cg->loopStack.pop_back();
        }
    };
    // RAII: a nested function body (a lambda, or a template instantiated mid-expression)
    // is a fresh control-flow context. Save and reset everything that belongs to the
    // enclosing body (defer/finally cleanups, loop and break/continue targets, the active
    // landingpad of a surrounding `try`), restoring it on scope exit, so the nested body
    // never runs the outer defers or branches/unwinds into the outer function's blocks.
    struct BodyContext {
        CodeGen* cg;
        std::vector<std::vector<Cleanup>> cleanups;
        size_t bcd, ccd;
        llvm::BasicBlock* bt; llvm::BasicBlock* ct; llvm::BasicBlock* unwind;
        std::vector<LoopFrame> loops;
        std::vector<TryCtx> tries;
        std::vector<llvm::Value*> catchTexts;
        explicit BodyContext(CodeGen* c)
            : cg(c), cleanups(std::move(c->cleanupScopes)),
              bcd(c->breakCleanupDepth), ccd(c->continueCleanupDepth),
              bt(c->breakTarget), ct(c->continueTarget), unwind(c->unwindTarget),
              loops(std::move(c->loopStack)), tries(std::move(c->tryStack)),
              catchTexts(std::move(c->catchTextSlots)) {
            cg->tryStack.clear();
            cg->catchTextSlots.clear();
            cg->cleanupScopes.clear();
            cg->breakCleanupDepth = cg->continueCleanupDepth = 0;
            cg->breakTarget = cg->continueTarget = nullptr;
            cg->unwindTarget = nullptr;
            cg->loopStack.clear();
        }
        ~BodyContext() {
            cg->cleanupScopes = std::move(cleanups);
            cg->breakCleanupDepth = bcd; cg->continueCleanupDepth = ccd;
            cg->breakTarget = bt; cg->continueTarget = ct;
            cg->unwindTarget = unwind;
            cg->loopStack = std::move(loops);
            cg->tryStack = std::move(tries);
            cg->catchTextSlots = std::move(catchTexts);
        }
    };
    // Emit (in LIFO order) every cleanup body in frames at index >= depth. On a normal
    // exit (errorPath=false) errdefer bodies are skipped; the `?`-propagation error path
    // passes errorPath=true so both run. Does not pop — the owning scope pops when it ends.
    void runCleanupsToDepth(size_t depth, bool errorPath);
    bool blockTerminated();   // is the current basic block already terminated?
    // Emit a statement body as its own scope (variables + defer cleanups), even when it
    // is a single unbraced statement.
    void emitScopedBody(const StmtPtr& body);
    // Address of element `idx` of an indexable base (fixed array / slice / pointer /
    // string). Shared by index-read, index-write (lvalue), and slice construction.
    llvm::Value* indexElemAddr(const ExprPtr& base, llvm::Value* idx, bool doCheck = true);
    llvm::Value* indexAsI64(const ExprPtr& e);

    // Exception handling: set when inside a try body
    llvm::BasicBlock* unwindTarget = nullptr;
    // Per enclosing `catch` handler: the entry alloca holding `exception_text()`'s string.
    std::vector<llvm::Value*> catchTextSlots;
    llvm::Function* ensureExceptionTextFn();

    // The C++ EH personality routine to reference for this target. mingw x86-64 unwinds via
    // SEH (`__gxx_personality_seh0`); Linux/macOS use the DWARF/Itanium `__gxx_personality_v0`.
    // Everything else about the EH lowering (landingpad IR + the `__cxa_*` runtime) is the same.
    std::string ehPersonalityName() const;

    // Helper: creates call or invoke depending on whether we are in a try body.
    // When in a try body, returns the invoke result and advances the insert point
    // to a fresh "normal continuation" block.
    llvm::Value* createMaybeInvoke(llvm::FunctionType* fty, llvm::Value* callee,
                                    llvm::ArrayRef<llvm::Value*> args,
                                    const llvm::Twine& name = "");

    // C ABI lowering for `extern` functions with by-value aggregate params/returns
    // (codegen_cabi.cpp). Each param/return is classified for the target: Direct
    // (unchanged), Coerce (one value of `ty`), Expand (the elements of the literal
    // struct `ty` as separate args), Indirect (pointer to a caller copy), ByVal
    // (pointer + byval), Sret (hidden result pointer).
    enum class CAbiTarget { None, AArch64, SysV, Win64, ARM32, X86, Wasm32 };
    struct CAbiArg {
        enum Kind { Direct, Coerce, Expand, Indirect, ByVal, Sret } kind = Direct;
        llvm::Type* ty = nullptr;
        unsigned align = 0;        // Indirect / ByVal / Sret alignment
        unsigned stackAlign = 0;   // `alignstack` for a stack-passed coerced arg (0 = none)
        unsigned offset = 0;       // Coerce: the byte offset of `ty` in the aggregate
    };
    struct CAbiSig {
        llvm::FunctionType* logical = nullptr;   // Eskiu-level signature
        llvm::FunctionType* lowered = nullptr;   // the declared C signature
        CAbiArg ret;
        std::vector<CAbiArg> params;
    };
    std::map<std::string, CAbiSig> externAbi;    // externs declared with a lowered signature
    CAbiTarget cabiTarget() const;
    // The unnamed-bitfield data ranges of `ty` placed at `base` (unnamedBitData, through
    // nested aggregates); an empty range marks one that holds no byte of the type.
    void cabiUnnamedData(llvm::Type* ty, uint64_t base,
                         std::vector<std::pair<uint64_t, uint64_t>>& out) const;
    // A Coerce value to and from the aggregate (cabiReinterpret at CAbiArg::offset).
    llvm::Value* cabiToCoerced(llvm::Value* v, const CAbiArg& a);
    llvm::Value* cabiFromCoerced(llvm::Value* c, const CAbiArg& a, llvm::Type* logical);
    void cabiLeaves(llvm::Type* ty, uint64_t base,
                    std::vector<std::pair<uint64_t, llvm::Type*>>& out) const;
    CAbiArg classifyCAbi(llvm::Type* ty, bool isReturn, CAbiTarget tgt,
                         unsigned& freeInt, unsigned& freeSSE) const;
    // 32-bit x86 (clang's X86_32ABIInfo): the C fields of an aggregate (a union's
    // members; false for a bitfield struct), whether it is returned in registers, and
    // the scalar a single-element struct is made of.
    bool x86Fields(llvm::Type* ty, std::vector<llvm::Type*>& out) const;
    bool x86RetInRegs(llvm::Type* ty) const;
    llvm::Type* x86SingleElement(llvm::Type* ty) const;
    // Fill `sig` for `logical`; false when no lowering is needed (no aggregate / target).
    bool buildCAbiSig(llvm::FunctionType* logical, CAbiSig& sig) const;
    void addCAbiAttrs(const CAbiSig& sig,
                      const std::function<void(unsigned, llvm::Attribute)>& add) const;
    llvm::Function* declareCAbiExtern(const std::string& name, const CAbiSig& sig);
    llvm::Value* cabiReinterpret(llvm::Value* v, llvm::Type* to);
    // Call a lowered extern with logical argument values; returns the logical result.
    llvm::Value* emitCAbiCall(llvm::Function* fn, const CAbiSig& sig,
                              const std::vector<llvm::Value*>& args, bool allowInvoke = true);
    // The address C should call for the Eskiu function `target` (a raw callback): the
    // function itself, or a `__cabi_<name>` thunk with the lowered C signature when it
    // takes or returns an aggregate by value.
    llvm::Function* cabiCallbackThunk(llvm::Function* target);
    llvm::Attribute::AttrKind cabiExtAttr(const std::string& eskiuType, llvm::Type* llty) const;
    // An `extern` parameter of fn type is a C function pointer (a bare `ptr`), per
    // extern: which parameters are. Functions the program defines are never lowered.
    std::map<std::string, std::vector<bool>> externFnPtrParams;
    std::map<std::string, std::vector<bool>> externVaListParams;
    // The C integer types of each extern's parameters (then its result) spelled
    // size_t, ssize_t or c_long, over all its declarations: 'z', 's', 'l' or 0.
    std::map<std::string, std::vector<char>> externCInts;
    std::map<std::string, char> externRetExt;   // a C result narrower than int64: 'z' or 's'
    static char cIntKind(const std::string& spelling);
    llvm::Type* cIntType(char kind);
    llvm::Value* widenCResult(const std::string& name, llvm::Value* v);
    void passLargeAggregatesByPointer(llvm::Module& m);
    void alignPackedAccesses(llvm::Module& m);
    // The declared type of the global whose initializer is being folded: a union literal
    // of that type holding an address narrower than its layout member (a pointer in an
    // int64-laid-out union on a 32-bit target) folds to a `{ ptr, [N x i8] }` literal.
    llvm::Type* splitUnionTop = nullptr;
    llvm::Value* evalCVaList(const ExprPtr& arg);
    llvm::Value* cVaListToEskiu(llvm::Value* c, llvm::Type* vaTy);
    llvm::Value* emitVaArg(llvm::Value* ap, llvm::Type* ty);
    std::set<std::string> definedFunctionNames;
    // The C function pointer passed for `arg` (a named top-level function or null).
    llvm::Value* evalCFnPointer(const ExprPtr& arg);

    // sret (structure return) support for large struct returns
    // Maps function name → actual return struct type (the LLVM function itself returns void)
    std::map<std::string, llvm::Type*> funcSretTypes;
    // Active sret pointer for the current function (null if not sret)
    llvm::Value* currentSretParam = nullptr;

    // Returns true if retType is an aggregate that must use sret on this target
    bool needsSret(llvm::Type* retType) const;

    // Type system: map Eskiu types to LLVM types
    llvm::Type* getTypeFromString(const std::string& typeStr);
    bool isPointerType(const std::string& typeStr) const;

    // Resolve the Eskiu type string of an expression (for struct/array access)
    std::string getExprEskiuType(const ExprPtr& expr) const;
    std::string bitfieldReadEskiu(const std::string& key, const std::string& member,
                                  const std::string& declType) const;
    llvm::Value* bitfieldReadValue(llvm::Value* v, const std::string& key, const std::string& member);
    std::string getExprEskiuTypeRaw(const ExprPtr& expr) const;   // before C promotion
    // The structural fallback: derive an expression's Eskiu type from the AST when
    // the single-resolver table has no entry. Split out so getExprEskiuType can,
    // under ESKIU_RESOLVER_DEBUG, cross-check the table against this derivation.
    std::string deriveExprEskiuType(const ExprPtr& expr) const;
    std::string deriveExprEskiuTypeUncached(const ExprPtr& expr) const;
    // While visit(BinaryExpr) handles a chain inside a template body (where operand types
    // are derived, and deriving a chain node derives the whole chain below it), the
    // derived types of the chain's own nodes, each computed once. Null otherwise.
    std::unordered_map<const Expr*, std::optional<std::string>>* chainTypeMemo = nullptr;

    // Expand a type alias to its underlying type string (peels pointers), so
    // downstream logic sees e.g. "*uint8" instead of an alias name like "Bytes".
    std::string expandAlias(const std::string& t) const;

    // Binary operators whose integer operands undergo C's integer promotions (to `int`
    // when narrower): arithmetic, bitwise, shifts, and comparisons.
    static bool isIntPromotingOp(const std::string& op);
    // True if the Eskiu type widens with zero-extension (unsigned / char / bool).
    bool eskiuUnsigned(const std::string& t) const;
    // Widen or truncate integer `val` to `ty`, choosing zero- vs sign-extension by
    // the source's signedness. The single place integer width coercion happens, so
    // an unsigned source never sign-extends (e.g. (int)(uint8)200 stays 200).
    llvm::Value* coerceInt(llvm::Value* val, llvm::Type* ty, bool unsignedSrc);
    llvm::Value* emitTruthy(llvm::Value* val);
    llvm::Type* pointerStrideType(const std::string& eskTy);
    std::string exceptionTypeName(const std::string& raw) const;

    // Integer->float conversion, choosing UIToFP vs SIToFP by source signedness.
    llvm::Value* intToFloat(llvm::Value* val, llvm::Type* ty, bool unsignedSrc);

    // Implicit numeric coercion of `val` to `target` (int/float widen/trunc/convert),
    // shared by every implicit-conversion site. Non-numeric values pass through.
    llvm::Value* coerceValue(llvm::Value* val, llvm::Type* target, bool unsignedSrc);

    // Reserve a stack slot in the *entry* block of the current function. All
    // allocas must live in the entry block: an alloca emitted inside a loop body
    // is re-run every iteration and its slot is not reclaimed until the function
    // returns, so a long-running loop with locals overflows the stack. Stores
    // still happen at the current insertion point; only the reservation hoists.
    llvm::AllocaInst* entryAlloca(llvm::Type* ty, llvm::Value* arrSize,
                                  const llvm::Twine& name = "");

    // Helper methods
    void pushScope();
    void popScope();
    llvm::Value* lookupSymbol(const std::string& name);
    void defineSymbol(const std::string& name, llvm::Value* value);

    // Visitor methods
    void visit(Program* node) override;
    void visit(FunctionDecl* node) override;
    void visit(VarDecl* node) override;
    void visit(StructDecl* node) override;
    void visit(ExternDecl* node) override;
    void visit(IntrinsicDecl* node) override;
    llvm::Value* lowerIntrinsicCall(const std::string& fn, class CallExpr* node);
    void visit(BlockStmt* node) override;
    void visit(IfStmt* node) override;
    void visit(ForStmt* node) override;
    void visit(ForInStmt* node) override;
    void visit(WhileStmt* node) override;
    void visit(DoWhileStmt* node) override;
    void visit(ReturnStmt* node) override;
    void visit(BreakStmt* node) override;
    void visit(ExprStmt* node) override;
    void visit(BinaryExpr* node) override;
    // Inside a template instantiation (whose body the type checker skips, so no opFunc
    // is stamped): resolve `op` over the operands' concrete types to a user operator
    // overload by its canonical mangled name. "" = a built-in operator.
    std::string resolveOpInTemplate(const std::string& op, const std::vector<ExprPtr>& operands) const;
    std::string resolveOpInTemplateTypes(const std::string& op,
                                         const std::vector<std::string>& operandTypes) const;
    // `lv op= v` with a side-effecting lvalue: evaluate lv's address once.
    void emitCompoundAssign(BinaryExpr* node, BinaryExpr* rhsOp);
    int compoundSeq = 0;
    // The pieces of visit(BinaryExpr): `lhs = rhs`, and one built-in operator applied to
    // an already evaluated left operand (the right one is evaluated here).
    void emitAssignment(BinaryExpr* node);
    llvm::Value* emitBuiltinBinary(BinaryExpr* node, llvm::Value* left);
    llvm::Value* emitIntDivRem(llvm::Value* left, llvm::Value* right, bool uns, bool rem);
    // `dbg(e)`: append the printf format and arguments that show the value at `addr`
    // (of Eskiu type `t`), structs field by field and arrays element by element.
    void dbgFormat(const std::string& t, llvm::Value* addr, std::string& fmt, std::vector<llvm::Value*>& args, int depth);
    llvm::Value* emitDbg(CallExpr* node);
    llvm::Value* safeArith(llvm::Intrinsic::ID id, llvm::Value* l, llvm::Value* r, bool uns);
    void emitSafeTrap(llvm::Value* cond, const char* name);
    llvm::Value* safeNonNull(llvm::Value* p);
    llvm::Value* maskShiftCount(llvm::Value* count);
    llvm::Value* emitFloatToInt(llvm::Value* val, llvm::Type* target, bool uns);
    void visit(UnaryExpr* node) override;
    void visit(IncDecExpr* node) override;
    void visit(QuestionExpr* node) override;
    void visit(TernaryExpr* node) override;
    void visit(CallExpr* node) override;
    void visit(IndexExpr* node) override;
    void visit(MemberExpr* node) override;
    void visit(CastExpr* node) override;
    void visit(LiteralExpr* node) override;
    void visit(IdentExpr* node) override;
    void visit(InterfaceDecl* node) override;
    void visit(ContinueStmt* node) override;
    void visit(SwitchStmt* node) override;
    void visit(MatchStmt* node) override;
    void visit(StructInitExpr* node) override;
    void visit(ArrayLitExpr* node) override;
    void visit(AllocWithExpr* node) override;
    void visit(TemplateCallExpr* node) override;
    void visit(LambdaExpr* node) override;
    // Emit the lambda's underlying LLVM function (shared by the runtime path in
    // visit(LambdaExpr) and the constant-closure path for global initializers).
    llvm::Function* emitLambdaFunction(LambdaExpr* node,
                                       const std::string& lambdaName,
                                       llvm::StructType* envTy);
    int lambdaSeq = 0;
    void visit(AsmStmt* node) override;
    void visit(UnionDecl* node) override;
    void visit(SizeofExpr* node) override;
    void visit(FreeClosureExpr* node) override;
    void visit(AwaitExpr* node) override;
    void visit(ThreadCreateExpr* node) override;

    // Union registry: name → fields (all share offset 0)
    std::map<std::string, std::vector<StructDecl::Field>> unionFields;
    // Union LLVM storage type → its members' LLVM types (the C-ABI classifier needs
    // every member, since the storage type keeps only the most-aligned one).
    std::map<llvm::StructType*, std::vector<llvm::Type*>> unionMemberTypes;
    void visit(ThreadJoinStmt* node) override;
    void visit(ThrowStmt* node) override;
    void visit(TryStmt* node) override;
    void visit(DeferStmt* node) override;
    void visit(EnumDecl* node) override;
    void visit(TypeAliasDecl* node) override;

    // `const` integer values, by name — so a const can be used as an array size.
    std::map<std::string, long long> constInts;
    // Folded values (in the declared type) of top-level `const` scalars, by name, so a
    // constant initializer evaluated before the globals exist can reference them.
    std::map<std::string, llvm::Constant*> constGlobalValues;
    // The folded value of each `const` scalar variable, keyed by its storage (alloca or
    // global): reading the variable yields the constant, so it folds in initializers
    // and case labels and is resolved through normal (shadowing-aware) name lookup.
    std::map<llvm::Value*, llvm::Constant*> constValueOf;
    // Nonzero while foldViaCodegen evaluates an expression for its constant value.
    int constEvalDepth = 0;
    // Evaluate `expr` with the ordinary expression codegen into a scratch function and
    // return the value if it folded to a constant (converted to `targetTy` when given),
    // else nullptr. So a constant initializer computes exactly what the same expression
    // computes at run time (promotions, signedness, wrapping).
    // With `asIface`, the value is converted to that interface (a boxed `&global`).
    llvm::Constant* foldViaCodegen(const ExprPtr& expr, llvm::Type* targetTy,
                                   const std::string& asIface = "");
    // Fold a numeric `const` declaration's initializer in its declared type, recording
    // it for array dimensions (constInts) and, at top level, by name. nullptr if not.
    llvm::Constant* foldConstDecl(VarDecl* v);
    // Resolve an array-dimension string (a decimal literal, an enum constant, or
    // a const int) to its value. Returns false if it cannot be resolved.
    bool resolveArrayDim(const std::string& dim, uint64_t& out) const;

    // Enum members -> integer value; the set of enum type names (each maps to i32)
    std::map<std::string, long long> enumConstants;
    std::set<std::string> enumTypes;
    // Algebraic enums: name -> decl (payloads) and variant -> (enum, tag). The
    // LLVM type lives in structTypes[enumName] as { i32 tag, [N x i64] payload }.
    std::map<std::string, EnumDecl*> adtEnumDecls;
    std::map<std::string, EnumDecl*> plainEnumDecls;   // classic int enums, for `match` on them
    std::map<std::string, std::pair<std::string, int>> adtVariants;
    // Generic algebraic enums (Option<T>): template decl + variant->(enum,tag), and
    // per-instance (Option_int) -> (generic name, concrete type args) for resolution.
    std::map<std::string, EnumDecl*> genericEnumDecls;
    std::map<std::string, std::pair<std::string, int>> genericVariants;
    std::map<std::string, std::pair<std::string, std::vector<std::string>>> enumInstanceArgs;
    // Build an algebraic-enum value for `variant`(args) (concrete enum; args may be empty).
    llvm::Value* buildVariant(const std::string& variant, const std::vector<ExprPtr>& args);
    // Core builder: { tag, payload } value with payload fields of `fieldTypes`.
    llvm::Value* buildEnumValue(llvm::StructType* et, int tag,
                                const std::vector<std::string>& fieldTypes, const std::vector<ExprPtr>& args);
    // Monomorphize a generic enum for `typeArgs`; returns the mangled instance name
    // (and creates its struct type + records enumInstanceArgs on first use).
    std::string ensureEnumInst(const std::string& genericName,
                               const std::vector<std::string>& typeArgs);
    // Names declared `intrinsic` — calls to these lower to inline IR, not a call.
    std::set<std::string> intrinsicNames;
    // Type aliases: alias name -> underlying type string
    std::map<std::string, std::string> typeAliases;

    void emitStructInitInto(llvm::Value* dest, StructInitExpr* init);
    // Field defaults (`struct S { int t = 30; }`): the expression a value of type `t` starts
    // as when nothing initializes it (`S{}` for a struct with defaults, directly or in a
    // field; an array literal of those for an array of it), or null for a zero value.
    ExprPtr defaultExprFor(const std::string& t);
    // `init` with every field it omits given its default (a named literal), or `init`
    // itself when the struct has no defaults.
    ExprPtr withDefaults(StructInitExpr* init, const ExprPtr& self);
    std::map<std::string, int> structHasDefaults;   // 1 yes, 0 no, -1 being computed
    // Fill an array alloca `dest` (of type `arrType`, e.g. "int[3]") from an array
    // literal: store each element, then zero-fill the remaining slots (C-style). With
    // `zeroed` the storage is already zero and only the given elements are stored.
    void emitArrayInitInto(llvm::Value* dest, ArrayLitExpr* lit, const std::string& arrType,
                           bool zeroed = false);
    // Zero `ty`'s storage at `dest` in one operation: memset, or a byte loop when
    // freestanding (no C library to call).
    void emitZeroFill(llvm::Value* dest, llvm::Type* ty);
    // Zero `size` (an i64 value) bytes at `dest`, the same two ways.
    void emitZeroBytes(llvm::Value* dest, llvm::Value* size, llvm::Align align);
    // An aggregate this large is zeroed by emitZeroFill rather than element by element.
    static constexpr uint64_t kBulkZeroBytes = 4096;
    // A constant array whose zero tail has this many elements is folded as a struct
    // `{ given..., [tail x T] zeroinitializer }` (constInitializer).
    static constexpr uint64_t kSplitZeroTail = 1024;
    // Resolve a struct-initializer name to a concrete struct type name, instantiating
    // the template if the name is of the form Name<Arg,...> (e.g. Pair<int,float>).
    std::string resolveStructInitName(const std::string& name);
    llvm::Function* getOrDeclareFunc(const std::string& name, llvm::Type* retType,
                                     std::vector<llvm::Type*> paramTypes, bool isVarArg = false);

    // Declare an Eskiu function prototype (no body) and register its sret/param
    // metadata. Idempotent: returns the existing llvm::Function if already created.
    // Used by visit(Program)'s prototype pre-pass so functions may be called before
    // they are defined (forward references, mutual recursion).
    llvm::Function* declareFunction(
        const std::string& name, const std::string& returnType,
        const std::vector<std::pair<std::string, std::string>>& params);
    // Create the LLVM struct type shell for a (non-template) struct. Idempotent.
    void declareStructType(StructDecl* node);
    void layoutStruct(const std::string& name, const std::vector<StructDecl::Field>& fields,
                      const std::vector<StructDecl::Pad>& pads, bool isPacked, int packAlign);
    // Manual layout at the C alignment of each field (cAlignOf), capped at packN for
    // #pragma pack(N>=2) (0 = no cap): used for a pack(N) struct and for one holding a
    // field LLVM would place at another offset. Fills `phys` with field types interleaved
    // with i8 padding and `slots` with one non-bitfield entry per field (physIndex into
    // `phys`); `align` is the struct's C alignment. False when a field is a bitfield.
    bool buildPackedLayout(const std::vector<StructDecl::Field>& fields, unsigned packN,
                           std::vector<llvm::Type*>& phys,
                           std::map<std::string, BitfieldSlot>& slots, uint64_t& align);

    // Expression evaluation (returns LLVM Value)
    std::stack<llvm::Value*> exprValueStack;
    llvm::Value* evaluateExpr(const ExprPtr& expr);
    llvm::Value* evaluateLValue(const ExprPtr& expr);
    // Address of `expr` for a READ (member access, indexing, a method receiver): like
    // evaluateLValue, but an rvalue aggregate (a call result, `a + b`, `c ? s : t`, a
    // struct literal) is materialized into a temporary, so `mk().a[1]` and `(a+b).y`
    // work. A store target still goes through the strict evaluateLValue.
    llvm::Value* evaluateAddress(const ExprPtr& expr);
    bool lvalueAllowTemp = false;   // set while evaluateAddress is resolving an address
};
