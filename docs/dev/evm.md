# EVM (EIR Virtual Machine)

EVM is the standalone interpreter for Eskiu Intermediate Representation (EIR), invoked with `eskiuc <file>.esk --run-eir`. It executes EIR modules directly in memory without invoking LLVM, emitting assembly, or calling an external system linker.

## 1. Overview

EIR decouples the Eskiu frontend from LLVM code generation (see [`eir-spec.md`](eir-spec.md)). While EIR serves as the common IR for multiple backends, EVM provides immediate, zero-compilation-delay execution:

```
Source (.esk) ──► Lexer ──► Parser ──► TypeChecker ──► EIRBuilder ──► EIR Module ──► EVM Execution
```

EVM interprets the EIR instruction stream using an explicit stack-frame architecture with native C FFI dispatch, emulating native execution semantics without the overhead of compilation or JIT translation.

## 2. Architecture & Runtime Model

### Stack Frames and Memory
- **Registers & Locals:** EVM allocates local variable storage (`Opcode::Alloca`) within contiguous stack frames (`StackFrame`). Large frame allocations are managed through chunked stack allocators to prevent host stack overflow during deep recursion.
- **Aggregates and Structs:** Struct layouts and total byte sizes are tracked via `eir::Module::structLayouts`. Global variables and return value buffers consult `getStructSize` and `getTypeSize(type, module)` to allocate exact layout bounds and preserve field offsets.
- **Fat Pointers:**
  - **Slices (`{ptr, len}`):** 16-byte pairs lowered through `Opcode::MakeSlice` and unpacked via `Opcode::ExtractFat`.
  - **Interfaces (`{data, vtable}`):** Dynamic dispatch fat pointers containing object instance pointers and resolved method tables.
  - **Closures (`{fn_ptr, env_ptr}`):** Function pointers paired with captured environment heap buffers.

### External Function Calls & FFI
External C library symbols (`printf`, POSIX syscalls, Windows Win32 APIs) are dispatched through dynamic symbol lookup (`dlsym` / host runtime address resolution) via `VM::callExtern`. Variadic arguments (`va_start`, `va_arg`, `va_end`) are handled natively inside the VM.

### Standard I/O and String Formatting
The VM provides integrated format string parsing (`formatString`) supporting standard integer, pointer, float, and string specifiers, along with variable width and precision arguments (`%*d`, `%.*s`).

## 3. Semantics and Test Suite Parity

EVM defines what an EIR program means, and the native back ends follow it
([`eir-backends.md`](eir-backends.md)): every integer value is held normalized to its type
(wrapped to its width, extended by its signedness), an `alloca` is zeroed each time it runs,
an aggregate is the address of its bytes, and integer `/` or `%` by zero stops the program
(`error: integer division by zero`, then abort) as the compiled program traps.

Most run tests (`tests/NAME.esk` with a `NAME.expected`) pass under `--run-eir`. The ones that
cannot are by design: inline assembly (the EVM does not execute it), a program that runs its
own binary again (`process_self`, the uncaught-exception report), a C companion object (no
linking), and a few network tests that exceed the run time under interpretation.

## 4. Scope and Design Boundaries

EVM serves as an immediate, in-memory semantic validator for EIR during compiler development and differential testing. Its scope is intentionally bounded:
- **In Scope:** Full execution of core EIR instructions (arithmetic, branches, calls, memory, alloca, slices, interfaces, closures, bitfields, exception unwinding) and standard C library FFI via `dlsym`.
- **Out of Scope (Non-Goals):** EVM does not attempt to be a full operating system or runtime emulator. Complex OS-level facilities (such as kernel-level asynchronous I/O event loops, signal-driven process supervisor trees, and high-performance server runtimes) are target domains for native compilation (`eskiuc -o prog` via native backends / LLVM).
