# EIR (Eskiu Intermediate Representation) Specification

This document defines the architecture, type system, instruction set, and textual representation of **EIR** (Eskiu Intermediate Representation). EIR is Eskiu's intermediate compiler representation, designed to decouple the frontend from LLVM and enable custom backends (native machine code emitters, C transpilers, and bytecode VMs).

---

## 1. Motivation and Design Goals

Today, the Eskiu compiler compiles through LLVM:
- The C++ compiler (`eskiuc`) uses the LLVM C++ API (`llvm::IRBuilder`, `llvm::Module`).
- The self-hosted compiler (`selfhost/`, `eskiuc-esk`) emits LLVM IR text and invokes `clang` to produce native binaries.

While LLVM provides world-class optimization for release binaries, relying exclusively on it introduces major drawbacks:
1. **Heavyweight dependency:** LLVM is millions of lines of code, slow to build, and massive in binary distribution size.
2. **Slow debug iterations:** Generating LLVM IR, running LLVM's internal passes, and emitting code through LLVM takes seconds for medium programs, whereas development workflows benefit from instant (<50ms) compilation.
3. **Self-hosting dependency:** The self-hosted compiler cannot ship as a standalone binary without requiring `clang` preinstalled on user machines.

### Goals of EIR
- **Backend agnostic:** Serve as the unified lowering target from Eskiu's frontend. Backends (LLVM, custom AArch64/x86-64 machine code emitter, C99 transpiler, or bytecode VM) consume EIR.
- **Explicit semantics:** High-level Eskiu features (structural interfaces as `{data, vtable}` fat pointers, closures as `{fn_ptr, env_ptr}`, slices as `{ptr, len}`, bitfields, and `defer` cleanup sequences) are lowered into explicit, simple EIR operations.
- **SSA-friendly with explicit stack frames:** Functions use explicit `alloca` for mutable variables, matching the AST's scoping. A straightforward Linear Scan or stack-spill register allocator can generate native assembly directly without complex Phi analysis, while an SSA transformation (mem2reg) remains straightforward.
- **Readable text representation:** EIR has a standard textual syntax (`.eir`) for debugging, diff testing (`--test-eir`), and tooling.

---

## 2. Pipeline Placement

EIR sits immediately after the frontend semantic passes and before backend code generation:

```
Source (.esk) ──► Lexer/Preproc ──► Parser ──► TypeChecker (1st pass)
                                                    │
                                                    ▼
                                              AsyncTransform
                                                    │
                                                    ▼
                                          TypeChecker (Single Resolver / post-TC)
                                                    │
                                                    ▼
                                         [ EIRBuilder (AST -> EIR) ]
                                                    │
                                                    ▼
                                              EIR Module
                                 ┌──────────────────┼──────────────────┐
                                 ▼                  ▼                  ▼
                           [ Native Backend ]  [ C Backend ]   [ LLVM Backend ]
                             (x86_64/ARM64)       (C99)         (via EIR/AST)
```

### Invariants Guaranteed to EIR
1. **Fully Typed AST:** Every expression's type is known via `postTc.expressionTypeMap()`.
2. **Monomorphized Generics:** Templates (`Result<int, string>`, `sort<T>`) and generic methods are already instantiated under unique names (e.g. `Result_int_string`).
3. **Desugared Async/Await:** `AsyncTransform` has already rewritten `async` functions into state-machine structs and step functions.
4. **Resolved Symbols:** Operator overloading functions (`__op_add_*`), implicit receiver methods (`T_method(self, ...)`), and constructors are resolved into concrete function calls.

---

## 3. Type System

EIR types are strictly defined and map directly from Eskiu's `ty::Type`:

### 3.1 Primitives
| EIR Type | Description | Bit Width / Representation |
|---|---|---|
| `void` | Absence of value | 0 bits |
| `i1` | Boolean | 1 bit (stored as 8-bit in memory) |
| `i8`, `u8` | Signed / Unsigned 8-bit integer | 8 bits |
| `i16`, `u16` | Signed / Unsigned 16-bit integer | 16 bits |
| `i32`, `u32` | Signed / Unsigned 32-bit integer | 32 bits |
| `i64`, `u64` | Signed / Unsigned 64-bit integer | 64 bits |
| `f32` | IEEE-754 single precision float | 32 bits |
| `f64` | IEEE-754 double precision float | 64 bits |
| `ptr` | Memory pointer | Target word size (32 or 64 bits) |

### 3.2 Aggregates and Composites
- **Struct:** `struct Name { T1, T2, ... }`
  Contains ordered fields with defined byte offsets and alignments.
- **Array:** `[N x T]`
  Fixed-size contiguous buffer of $N$ elements of type $T$.
- **Function Prototype:** `fn(P1, P2, ...) -> R`
  Signature representing a direct or indirect function pointer.

### 3.3 Fat Pointers (First-Class Aggregates)
Eskiu uses fat pointers extensively. In EIR, these are modeled either as built-in compound types or canonical two-word structs:
- **Interface Fat Pointer:** `{ ptr data, ptr vtable }`
- **Closure Fat Pointer:** `{ ptr fn_ptr, ptr env_ptr }`
- **Slice Fat Pointer:** `{ ptr data_ptr, i64 len }`

---

## 4. Program Structure

An EIR translation unit is an `eir::Module`:

```
Module
 ├── Target Metadata (Triple, DataLayout)
 ├── Type Definitions (Structs, Unions, Interfaces)
 ├── Global Variables & Constants
 ├── External Function Declarations (extern C)
 └── Function Definitions
      └── Basic Blocks
           └── Instructions (terminating in Br, BrCond, Ret, etc.)
```

### 4.1 Identifiers and Values
- **Globals & Functions:** Prefixed with `@` (e.g. `@printf`, `@main`, `@global_table`).
- **Virtual Registers (SSA temporaries / locals):** Prefixed with `%` (e.g. `%0`, `%x`, `%ptr`).
- **Basic Block Labels:** Prefixed with `$` or plain label name (e.g. `entry:`, `then:`, `loop.body:`).
- **Constants:** Literals formatted directly (e.g. `42`, `-10`, `3.14159`, `"Hello\n"`, `null`, `true`, `false`).

---

## 5. Instruction Set

EIR instructions are 3-address operations. Every value-producing instruction assigns to a virtual register `%dst`.

### 5.1 Memory & Storage
* `alloca %dst, <type>, align <N>`
  Allocates space on the current function's stack frame. Returns a `ptr`.
* `load %dst, <type>, %ptr` / `load volatile %dst, <type>, %ptr`
  Loads a value of type `<type>` from memory address `%ptr`.
  When qualified with `volatile` (`isVolatile == true`), backends and optimization passes must never reorder, elide, cache in registers, or merge this memory access. Critical for MMIO (freestanding/bare-metal kernel and vision hardware buffers).
* `store <type> %val, %ptr` / `store volatile <type> %val, %ptr`
  Stores `%val` into memory address `%ptr`.
  When qualified with `volatile`, guarantees an exact store to memory that cannot be eliminated as dead code or reordered across sequence points.
* `gep %dst, <elem_type>, %ptr, %idx`
  Calculates pointer offset for array/pointer indexing: `%ptr + %idx * sizeof(elem_type)`.
* `offset_ptr %dst, ptr %ptr, <byte_offset>`
  Calculates byte-addressed pointer offset `%ptr + <byte_offset>`. Used for struct field and bitfield storage access.
* `field_ptr %dst, <struct_name>, %ptr, <field_idx>`
  Calculates pointer to field `<field_idx>` of struct `<struct_name>` at address `%ptr`.

### 5.2 Arithmetic & Bitwise
Operates on integer and floating point registers:
* Integer binary: `add`, `sub`, `mul`, `sdiv`, `udiv`, `srem`, `urem`
* Floating-point binary: `fadd`, `fsub`, `fmul`, `fdiv`
* Bitwise binary: `and`, `or`, `xor`, `shl`, `lshr` (logical shift right), `ashr` (arithmetic shift right)
* Unary: `neg`, `not`

Syntax:
```eir
%2 = add i32 %0, %1
%3 = fmul f64 %fa, %fb
%4 = and u32 %a, 0xFF
```

Both operands have the instruction's type, and the result is normalized to it (wrapped
to the width, sign- or zero-extended by the signedness). C's promotions and usual
arithmetic conversions are explicit `sext`/`zext`/`trunc`/`sitofp`/`fpext` before the
instruction; a shift's count has the shifted operand's type and is masked to its width.
Integer `sdiv`/`udiv`/`srem`/`urem` by zero stop the program.

### 5.3 Comparisons & Conditionals
* `icmp <cond> <type> %a, %b`
  Integer comparison. Condition `<cond>`:
  `eq`, `ne`, `slt`, `sle`, `sgt`, `sge` (signed), `ult`, `ule`, `ugt`, `uge` (unsigned).
  Returns `i1`.
* `fcmp <cond> <type> %a, %b`
  Float comparison: `oeq`, `one`, `olt`, `ole`, `ogt`, `oge`, etc.
  Returns `i1`.

### 5.4 Conversions & Casts
* `trunc <from_ty> %val to <to_ty>`: Integer truncation.
* `zext <from_ty> %val to <to_ty>`: Zero extension.
* `sext <from_ty> %val to <to_ty>`: Sign extension.
* `fptrunc <from_ty> %val to <to_ty>`: Float double -> float.
* `fpext <from_ty> %val to <to_ty>`: Float float -> double.
* `fptosi <from_ty> %val to <to_ty>`: Float to signed integer.
* `sitofp <from_ty> %val to <to_ty>`: Signed integer to float.
* `bitcast <from_ty> %val to <to_ty>`: Reinterpret bits (e.g. pointer to pointer).
* `ptrtoint %val to i64`: Pointer to integer address.
* `inttoptr %val to ptr`: Integer address to pointer.

### 5.5 Control Flow & Terminators
Every basic block MUST end in exactly one terminator:
* `ret <type> %val` / `ret void`: Return from current function.
* `br $target`: Unconditional jump to basic block `$target`.
* `br_cond i1 %cond, $then_bb, $else_bb`: Conditional jump.
* `switch <type> %val, $default_bb [ <c1>: $bb1, <c2>: $bb2, ... ]`: Multi-way jump.
* `unreachable`: Declares unreachable state (used after traps or infinite loops).

### 5.6 Calls and Fat Pointers
* `call %dst, <ret_ty> @fn_name( <arg_ty> %a1, ... )`
  Direct function call.
* `call_indirect %dst, <ret_ty> %fn_ptr( <arg_ty> %a1, ... )`
  Indirect call through function pointer.
* `make_interface %dst, ptr %data, ptr %vtable`
  Constructs an interface fat pointer.
* `make_closure %dst, ptr %fn_ptr, ptr %env_ptr`
  Constructs a closure fat pointer.
* `make_slice %dst, ptr %data_ptr, i64 %len`
  Constructs a slice fat pointer.
* `extract_fat %dst, <fat_val>, <index>`
  Extracts field 0 (data/fn_ptr) or field 1 (vtable/env_ptr/len) from a fat pointer.

### 5.7 Exceptions & Unwinding
* `throw ptr %exception_obj`
* `invoke %dst, <ret_ty> @fn_name( <args> ) to $ok_bb unwind $catch_bb`
* `landingpad %dst`: Extracts exception info in the landing block.

### 5.8 Inline Assembly
* `inline_asm "<template>", "<constraints>", [<operands>...] ~[<clobbers>...]`
  Direct inline assembly instruction for low-level architecture control, bare metal ARM64/x86 kernel development, and SIMD/MMIO operations.
  - Template: String containing raw assembly instructions (e.g. `"svc #0"`, `"mrs %0, vbar_el1"`, `"dsb sy"`).
  - Constraints: GCC/LLVM-style operand constraints (e.g. `"=r"`, `"r"`, `"+r"`, `"m"`).
  - Operands: Virtual registers passed as inputs/outputs.
  - Clobbers: List of clobbered registers or memory barriers (e.g. `"memory"`, `"cc"`).
  - Example:
    ```eir
    inline_asm "str %1, [%0]", "r,r", ptr %addr, i32 %val ~["memory"]
    ```

### 5.9 Explicit Lowering for Bitfields
EIR does not contain high-level bitfield types or instructions. Instead, struct bitfields are packed into primitive storage units (`u8`, `u16`, `u32`, `u64`) following target C ABI rules (AAPCS/SysV/MSVC) and lowered into explicit elementary memory and bitwise operations.

#### Bitfield Read Lowering:
1. **Locate storage unit:**
   ```eir
   %unit_ptr = offset_ptr ptr %base_ptr, <storage_byte_offset>
   ```
2. **Load unit (inheriting volatile if base pointer or member is volatile):**
   ```eir
   %raw = load [volatile] <storage_type>, ptr %unit_ptr
   ```
3. **Shift out lower bits:**
   ```eir
   %shifted = lshr <storage_type> %raw, <bit_offset>
   ```
4. **Mask field width:**
   ```eir
   %masked = and <storage_type> %shifted, <(1 << bit_width) - 1>
   ```
5. **Sign extension (for signed bitfields):**
   If the bitfield is signed, the field is shifted left by `(storage_bits - bit_width)` and arithmetic-shifted right by the same amount:
   ```eir
   %tmp = shl <storage_type> %masked, <storage_bits - bit_width>
   %val = ashr <storage_type> %tmp, <storage_bits - bit_width>
   ```
6. **Promote to expression type:**
   C promotion rules apply (`zext` or `sext` to `i32`/`i64`).

#### Bitfield Write Lowering:
1. **Locate storage unit:**
   ```eir
   %unit_ptr = offset_ptr ptr %base_ptr, <storage_byte_offset>
   ```
2. **Load existing storage unit:**
   ```eir
   %orig = load [volatile] <storage_type>, ptr %unit_ptr
   ```
3. **Clear field bits:**
   Compute mask $M = \sim(((1 \ll \text{bit\_width}) - 1) \ll \text{bit\_offset})$:
   ```eir
   %cleared = and <storage_type> %orig, <M>
   ```
4. **Truncate & mask new value, then shift into position:**
   ```eir
   %val_trunc = trunc <val_type> %new_val to <storage_type>
   %val_masked = and <storage_type> %val_trunc, <(1 << bit_width) - 1>
   %val_shifted = shl <storage_type> %val_masked, <bit_offset>
   ```
5. **Merge and store back:**
   ```eir
   %merged = or <storage_type> %cleared, %val_shifted
   store [volatile] <storage_type> %merged, ptr %unit_ptr
   ```

### 5.10 Explicit Lowering for `defer` and Scope Cleanup Sequences
EIR lowers `defer` statements and scope-exit cleanup actions into explicit control flow blocks with zero overhead for successful paths:

#### 1. Synchronous & Structured Control Flow (Inlined LIFO):
* Each lexical block (`BlockStmt`, `WhileStmt`, `ForStmt`, `TryStmt`) maintains a compile-time cleanup stack.
* When a `defer` statement is encountered, its body is parsed and registered in the current scope's cleanup stack.
* **Normal block exit (fallthrough):**
  Cleanups registered in that lexical scope are emitted sequentially in strict LIFO order before transitioning or jumping to the successor basic block.
* **Early exits (`return`, `break`, `continue`):**
  Before emitting `ret` or `br`, EIRBuilder iterates and inlines all pending cleanups from the current inner scope up to the target scope (e.g. up to the function root for `return`, or up to the loop scope for `break`/`continue`).

#### 2. Exceptional Control Flow (Landing Pads & Unwinding):
* Calls capable of throwing exceptions inside a scope with active cleanups or `catch`/`finally` handlers are emitted as `invoke ... to $normal_bb unwind $cleanup_bb`.
* `$cleanup_bb` begins with:
  ```eir
  %ex = landingpad ptr
  ```
* Pending cleanups for the scope are executed in LIFO order inside the landingpad block.
* If an enclosing `catch` handler matches, control branches to the handler basic block.
* If no handler matches in the function, control terminates in `throw ptr %ex` (or backend rethrow / `_Unwind_Resume`) to propagate the exception up the call stack.
* Functions without `defer`, `finally`, or non-trivial destructors emit standard `call` instructions without landing pad overhead.

---

## 6. Textual Syntax Example

Consider this Eskiu program:

```eskiu
fn add(int a, int b) -> int {
    let sum = a + b;
    if (sum > 10) {
        return sum * 2;
    }
    return sum;
}
```

The lowered **EIR** (`.eir`) representation:

```eir
module "example"

fn @add(i32 %a, i32 %b) -> i32 {
entry:
    %a.addr = alloca i32, align 4
    %b.addr = alloca i32, align 4
    %sum.addr = alloca i32, align 4
    store i32 %a, ptr %a.addr
    store i32 %b, ptr %b.addr

    %0 = load i32, ptr %a.addr
    %1 = load i32, ptr %b.addr
    %2 = add i32 %0, %1
    store i32 %2, ptr %sum.addr

    %3 = load i32, ptr %sum.addr
    %4 = icmp sgt i32 %3, 10
    br_cond i1 %4, $then, $else

then:
    %5 = load i32, ptr %sum.addr
    %6 = mul i32 %5, 2
    ret i32 %6

else:
    br $merge

merge:
    %7 = load i32, ptr %sum.addr
    ret i32 %7
}
```

---

## 7. Implementation Status

1. **EIR data structures** (`eir/eir.h`, `eir/eir.cpp`): types, operands, instructions, blocks,
   functions, modules and the text printer (`--test-eir`). Done.
2. **EIR builder** (`eir/eir_builder.{h,cpp}`): an `ASTVisitor` over the checked AST, typing
   each expression from the type checker (per instance inside a generic). Done.
3. **EVM** (`eir/eir_vm.cpp`, `--run-eir`): see [`evm.md`](evm.md). Done.
4. **Native back ends** (`--emit-asm` ARM64, `--emit-c` C99, `--emit-obj`, `--backend=eir`):
   see [`eir-backends.md`](eir-backends.md); both pass the run-test corpus, gated by
   `tests/eir/check.sh`. Done.
5. **Self-hosted parity**: `selfhost/eir.esk` printing the same `--test-eir` text as the C++
   builder for every test. In progress (`tests/selfhost/eir_parity.sh`).
