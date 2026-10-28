# EIR native backends

Besides the EVM interpreter ([`evm.md`](evm.md)), EIR has two backends that produce native
code without LLVM's code generator:

| Mode | Output |
|---|---|
| `eskiuc f.esk --emit-asm [-o f.s]` | ARM64 assembly (`eir/eir_asm_arm64.cpp`), Mach-O or ELF syntax by target |
| `eskiuc f.esk --emit-c [-o f.c]` | C99 (`eir/eir_c.cpp`) |
| `eskiuc f.esk -o prog --backend=eir` | an executable: the normal driver (link, `#pragma link`, implied libraries, `.a` output, `run`) with the object from EIR |
| `eskiuc f.esk --emit-obj [-o f.o]` | the object file alone |

`--backend=eir` builds the object through the system C driver (`$CC`, `cc`, `clang`, `gcc`):
it assembles the ARM64 output on an arm64 target (Windows excepted) and compiles the C99
output on any other (`eir/eir_obj.cpp`). Options that only change LLVM's code generation
(`-O`, `--safe`, `--freestanding`, `--asan`, `--ubsan`, `--mcpu`, `--mattr`, `--reloc`) are
errors under it, not ignored.

## Value model

Both backends follow the EVM, which defines what an EIR program means:

- An integer value is 64 bits wide. A 32-bit `add`/`sub`/`mul`/`sdiv`/`srem` wraps to 32
  bits, a narrow unsigned bitwise result is masked to its width, a shift works at its
  type's width with the count masked; everything else is 64-bit. A load extends by its
  type, a store truncates. An integer constant is used as written.
- An aggregate (struct, array, closure, interface, slice) is the address of its bytes. A
  load of one yields the address, a store copies the bytes.
- Integer `/` and `%` stop the program on a zero divisor (`brk` / `__builtin_trap`; the EVM
  aborts with an error). `MIN / -1` wraps.
- Float to integer saturates and NaN is 0.
- An `icmp` on an interface compares its data pointers.

Function boundaries follow the C ABI (AAPCS64 for assembly): parameters and results have
their exact types, so a function can be passed to C (`qsort`). A function returning an
aggregate gets the caller's buffer (x8 in assembly, a first `void*` parameter in C) and
returns its address. An extern cannot take or return a struct by value under these
backends; that is a compile error. A closure's function always takes the env first: a
top-level function used as a closure is reached through `__fnptr_<name>` (built in
`EIRBuilder::closureTarget`), and an extern's function parameter receives the plain C
function pointer.

## Runtime

What the EVM does internally is a small C runtime (`eir/eir_runtime.cpp`): the pending
exception and the caught stack (thread-local), `exception_text`, the uncaught-exception
report (`error: uncaught exception: T v`, then abort, as the LLVM build), and threads
(`pthread`, `CreateThread` on Windows). The assembly path compiles it as a second object
and joins the two with `cc -r`; the C output carries it inline, calling each C function
through a cast so that a program declaring the same function with other types does not
conflict.

Exceptions are not table-driven unwinding. A frame keeps a stack of handler ids
(`PushHandler`/`PopHandler`); a `throw` records the exception and jumps to the frame's
unwind block, which resumes at the innermost handler or returns; after each call that may
throw, the caller checks for a pending exception. A program with no `throw` pays nothing.

## Testing

`tests/eir/check.sh [asm|c]` compiles every run test with the backend, runs it, and
compares its output; `tests/eir/pass_<mode>.txt` lists the tests known to pass, and the
script fails when one of them stops passing (`UPDATE=1` rewrites the list). Tests that fail
under both the EVM and the backends point at gaps in the EIR builder, not the backends.
