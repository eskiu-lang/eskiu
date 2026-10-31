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
output on any other (`eir/eir_obj.cpp`, `eirUsesAsm`). On the C path `-O` is the C
compiler's level (the C is always compiled with `-fno-strict-aliasing`); the ARM64 path has
none, so `-O` is an error there. Options that only change LLVM's code generation
(`--safe`, `--freestanding`, `--asan`, `--ubsan`, `--mcpu`, `--mattr`, `--reloc`) are errors
under it, not ignored.

## Value model

The EVM and both backends share one meaning of an EIR program:

- An integer value is normalized to its type: wrapped to its width and sign- or
  zero-extended by its signedness. Every instruction producing an integer normalizes its
  result, and an integer constant is read as a value of its operand type. A load extends by
  its type, a store truncates.
- The operands of an arithmetic, bitwise or comparison instruction have the instruction's
  type. The builder makes C's conversions explicit (`EIRBuilder::promoteInt`,
  `commonType`, `coerceNumeric`): a narrow integer promotes to `int`, two operands meet at
  their usual arithmetic conversion type, a shift has its promoted left operand's type, a
  compound assignment converts its result back to the target, and a value passed,
  returned, stored or tested converts to the type there. Constant expressions fold with
  `ty::CInt`, as sema and codegen fold them.
- An aggregate (struct, array, closure, interface, slice) is the address of its bytes. A
  load of one yields the address, a store copies the bytes. An `alloca` is zeroed each time
  it runs.
- Integer `/` and `%` stop the program on a zero divisor (`brk` / `__builtin_trap`; the EVM
  aborts with an error). `MIN / -1` wraps.
- Float to integer saturates and NaN is 0.
- An `icmp` on an interface compares its data pointers.

Function boundaries follow the C ABI (AAPCS64 for assembly): parameters and results have
their exact types, so a function can be passed to C (`qsort`). Between Eskiu functions a
record (struct, union, closure, interface, slice) travels by address, and a function
returning one writes it to the buffer its caller passes (x8 in assembly, a first `void*`
parameter in C) and returns that address. A closure's function always takes the env
first: a top-level function used as a closure is reached through `__fnptr_<name>` (built
in `EIRBuilder::closureTarget`), and an extern's function parameter receives the plain C
function pointer.

A struct or union passed to or returned from an `extern` by value follows the C ABI. The C
backend spells the record in C (`struct __eskc_r_<name>`, its unions, bitfields, unnamed
bitfields and `#pragma pack` as declared) and lets the C compiler pass it. The assembly
backend classifies it as AAPCS64 does (`classifyC`): a homogeneous floating-point aggregate
(one to four members of one float type; an unnamed bitfield of nonzero width breaks one) in
consecutive v registers, else up to 16 bytes in x registers, else by address of a copy; the
stack when the registers run out; a result in v0-v3, x0/x1, or the buffer x8 points to. A
function handed to C by address whose signature has a record or a `va_list` is reached
through a thunk with the C signature (`__cabi_<name>`, `__eskc_cabi_<name>`).

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
