# Eskiu ABI

How Eskiu types and constructs lower to LLVM IR, and the calling/layout
conventions a foreign caller must follow. This reflects the code generator
(`codegen/codegen_*.cpp`); it is the contract for linking Eskiu with C (and, on
the roadmap, Swift/Kotlin over the C ABI).

Eskiu targets the platform's native C ABI via LLVM, so on a given target the
representations below match what a C compiler produces for the equivalent
declarations. There is no separate Eskiu calling convention.

---

## Scalar types

| Eskiu | LLVM | Signed? | Notes |
|-------|------|---------|-------|
| `int`, `int32` | `i32` | signed | default integer width |
| `int8` / `int16` / `int64` | `i8` / `i16` / `i64` | signed | |
| `uint`, `uint32` | `i32` | unsigned | |
| `uint8` / `uint16` / `uint64` | `i8` / `i16` / `i64` | unsigned | |
| `bool` | `i1` | n/a | |
| `char` | `i8` | unsigned | as a value; at an `extern` boundary its extension follows the target's C `char` (see *Narrow integers*) |
| `float` | `float` | n/a | IEEE-754 single |
| `double` | `double` | n/a | IEEE-754 double |
| `string` | `ptr` | n/a | pointer to a NUL-terminated byte buffer |
| `void` | `void` | n/a | return type only |

Signedness is not part of the LLVM type (both `int` and `uint` are `i32`); it
is carried by the operations. Integer **widening** chooses the extension by the
*source* type: `uint*`, `char` and `bool` zero-extend; signed integers
sign-extend. Narrowing truncates. Integer↔float conversions follow the integer's
signedness (`sitofp`/`fptosi` for signed, `uitofp`/`fptoui` for unsigned).

---

## Pointers and `const`

All pointers lower to LLVM's opaque `ptr` (address space 0), regardless of
pointee type or spelling (`int*` and `*int` are identical). Pointer-to-pointer,
casts between pointer types, and `string` are all just `ptr`. A checked nullable
pointer `?*T` has the identical representation to `*T` (a bare `ptr`); its
non-null requirement is enforced by the type checker at compile time and adds no
runtime cost or storage.

A leading `*` and a trailing `[N]` follow the source rule that the array binds
outermost: `*T[N]` is an array of N `ptr` elements. There is no pointer-to-array
spelling (`T[N]*` does not parse); a `*T` to the first element is the single `ptr` C
code expects for a `T (*)[N]`. For C interop, libc `size_t` parameters and returns
(`strlen`, the size argument of `memcpy`/`memset`/`memmove`/`memcmp`/`memchr`) are
declared `int64` so the extern signatures match the C ABI on 64-bit targets.

`const` has **no ABI effect**. Every `const` qualifier (base/pointee `const T`,
pointer-level `T* const`) is stripped before lowering, so a `const int*` and an
`int*` are the same at the IR/object level. const is purely a compile-time
checking aid.

---

## Aggregates

### Structs

A non-packed struct without bitfields lowers to an LLVM struct of its field
types in declaration order, with the target's **natural alignment** (LLVM
inserts padding). This matches a C `struct` with the same fields.

### Packed structs

`packed struct` (equivalently `#pragma pack(1)`) lowers to an LLVM *packed*
struct: no padding, fields back-to-back.

`#pragma pack(N)` for `N > 1` uses an explicit manual layout that caps each
field's alignment at `N`: a field is placed at the next offset that is a
multiple of `min(natural-align, N)`, with `[k x i8]` padding inserted as needed,
and the total size rounded up to the struct's alignment (`min(max-field-align,
N)`). The result is emitted as a packed LLVM struct (padding is explicit) and
matches the C `#pragma pack(N)` ABI. Field access goes through a
logical→physical index map (padding shifts indices).

LLVM sees such a struct as 1-aligned, so its C alignment (`min(max-field-align,
N)`) is recorded beside the type (`cAlignOverride` / `cg_set_calign`) and read
through `cAlignOf` / `cg_c_align` wherever C alignment matters: a struct with a
field (or array element) whose C alignment is above LLVM's is laid out the same
way by hand (no cap) and records its own alignment, and so do a bitfield struct
and a union holding one; the C ABI lowering classifies the aggregate with that
alignment and takes the leaves of a hand-laid struct from its fields, not its
padding runs. The type checker's `constLayout` / `sema_const_layout` fold
`sizeof` with the same alignment.

### Bitfields

A struct with bitfields is laid out the way the target's C compiler does it.

- **SysV / AAPCS targets** (Linux, macOS, bare-metal ARM; clang's Itanium
  layout): a bitfield takes the next free bit unless that would make it cross a
  boundary of a storage unit of its declared type (a `uint32` field must fit in
  an aligned 4-byte unit), in which case it starts at that boundary. So adjacent
  bitfields of different declared types share bytes (`uint8 a:4; uint32 w:12;`
  is 4 bytes, `w` at bits 4..15), and a normal field starts at the next free
  byte, aligned, possibly inside a bitfield's unit (`uint32 a:4; char c;` puts
  `c` at offset 1). The struct is aligned to its most aligned field, bitfields
  included. In a `packed struct` or under `#pragma pack(N)`, bitfields are
  packed back to back with no unit rule.
- **Windows targets** (MS layout): consecutive bitfields share a storage word
  of their declared type while the type size stays the same and the next one
  fits; a new word opens when the declared type size changes or the field does
  not fit, and a normal field closes the current word. Under `#pragma pack(N)`
  each word and field is aligned to at most N.

Under the SysV/AAPCS layout every field of such a struct is addressed by byte
offset. The LLVM type only reproduces the C size and alignment: each storage
unit as an integer of its declared type, the normal fields outside those units
as themselves, and `[n x i8]` for the remaining bytes (a packed struct is a
packed LLVM struct of normal fields and byte runs). Reads load the bitfield's
storage unit (in a packed struct, the exact byte span, e.g. `i24`), shift by the
bit offset and mask (sign-extending for signed fields); writes, compound
assignments and `++`/`--` are read-modify-write of that unit.

### Unions

A `union` lowers to `{ M, [P x i8] }`, where `M` is its most-aligned member
(ties go to the larger one) and the byte padding brings the size up to the
largest member rounded to that alignment. So the union has C's size and
alignment, and lands at the C offset inside a struct (`struct { int tag; union {
int64 l; int i; } u; }` puts `u` at 8). All members share offset 0; a member
access reinterprets the storage at that type. Under `#pragma pack(N)` each member's
alignment is capped at `N` (as C); when that lowers the union's alignment the storage
is the packed `<{ M, [P x i8] }>` with its C alignment recorded (`cAlignOverride` /
`cg_set_calign`), so it lands at the C offset in a struct and the C ABI lowering sees
the C size and alignment.

---

## Enums

### Classic enums

A payload-less `enum` is an `i32`. Each member is a compile-time `i32` constant
(explicit `= value` or the running auto-increment).

### Algebraic enums (ADTs)

A payload-bearing enum is a tagged union:

```
{ i32 tag, [N x i64] payload }
```

- `tag` is the 0-based variant index (`i32`).
- `payload` is `[N x i64]`, where `N = ceil(maxPayloadBytes / 8)` (at least 1),
  `maxPayloadBytes` being the largest variant's naturally-aligned field layout.
- Construction stores the tag, then writes each variant field into the payload
  area (coercing the value to the field type). `match` reads the tag, branches,
  and reads payload fields per arm.

Generic ADTs (`Option<T>`, `Either<A,B>`) are monomorphized per instantiation:
each concrete instance (`Option_int`) gets its own `{i32, [N x i64]}` sized for
that instance, exactly like template structs.

---

## Functions and calling convention

Parameters and results use the platform C ABI as produced by LLVM for the
lowered types. Scalars and pointers pass in registers per the target ABI.

**Struct return (sret).** A function returning a struct larger than **16 bytes**
(`getTypeAllocSize > 16`) returns `void` and takes a hidden pointer as its first
parameter; the caller allocates the result buffer and passes its address.
Structs ≤ 16 bytes are returned by value (the target ABI splits them into
registers as usual). This is the convention between Eskiu functions; calls to
`extern` C functions follow the C ABI lowering below.

**Aggregates across `extern` (C ABI).** Between Eskiu functions a struct or
union argument/result is a first-class LLVM value. An `extern` function that
takes or returns one by value is instead declared with the lowered C signature
(`codegen/codegen_cabi.cpp`, mirrored by the self-hosted codegen), and each call
converts to and from it, so it links against C compiled by clang/gcc:

| Target | Aggregate argument | Aggregate result |
|---|---|---|
| AArch64 (AAPCS64, Darwin + Linux) | HFA of 1-4 `float`/`double` → `[N x fp]` in FP registers (with `alignstack(8)` off Darwin); ≤ 8 bytes → `i64`; ≤ 16 → `[2 x i64]`; larger → pointer to a caller-made copy | HFA → `{ fp, ... }`; ≤ 8 bytes → `iN`; ≤ 16 → `[2 x i64]`; larger → `sret` (x8) |
| x86-64 System V (Linux, macOS) | each eightbyte classified INTEGER/SSE → one or two register values (`i64`, `i32`, `double`, `<2 x float>`, `ptr`, ...), as wide as the data in it (a union's widest member decides: `union { int; double; }` is `i64`, `union { float; double; }` is `double`); > 16 bytes, a misaligned field, or no free registers left → `byval` | the same classes as a `{ lo, hi }` pair or one value; > 16 bytes → `sret` |
| Windows x64 | size 1/2/4/8 → `iN`; otherwise pointer to a caller-made copy | size 1/2/4/8 → `iN`; otherwise `sret` |
| 32-bit ARM (AAPCS) | hard-float HFA → `{ fp, ... }`; ≤ 64 bytes → `[N x i32]` (`[N x i64]` if 8-aligned); larger → `byval` | hard-float HFA → `{ fp, ... }`; ≤ 4 bytes → `i32`; otherwise `sret` |
| 32-bit x86 (cdecl: i386 SysV, Darwin, Windows) | ≤ 16 bytes made only of 32/64-bit scalars (`int`, `int64`, `float`, `double`, pointers) with no padding → those scalars as separate arguments (`{ int, double }` → `i32, double`; not on Windows, where `double` is 8-aligned and leaves padding); anything else → `byval` in a 4-byte-aligned stack slot | Linux (and other SysV i386 systems): always `sret`. Darwin, Windows and the BSDs: 1, 2, 4 or 8 bytes whose fields are each register-sized → `iN` in EAX:EDX, except a single-element struct of a `float`/`double` (in ST0; not under MSVC's rules, mingw keeps it) or a pointer, returned as that scalar; otherwise `sret` |
| wasm32 (emscripten, WASI) | a single-element struct (one scalar through one-field structs, one-element arrays and one-member unions) → that scalar; anything else → `byval` at its alignment | single-element struct → that scalar; otherwise `sret` |

The coerced types match what clang emits for the same C signature
(`tests/selfhost/cabi_parity.sh` checks the 32-bit x86 and wasm32 ones against clang's IR
for the tests' C companions). Other targets keep the first-class lowering. Fat values (closures, slices) and
`va_list` are not C aggregates and keep their own layout.

Where gcc and clang disagree, Eskiu follows clang. The one known case: since GCC 12, gcc on
x86-64 classifies a struct holding a zero-width bitfield (`uint32 : 0;`) differently when it
is passed or returned by value, so such a struct crossing an `extern` matches C compiled by
clang, not by GCC 12 or later.

**Target-width C integers.** `size_t`, `ssize_t` and `c_long` (`<ctypes>`) are `int64`
aliases in Eskiu. In an `extern`'s parameters and result the compiler declares the target's
C type instead (`cIntKind`/`cIntType` / self-host `cg_cint_kind`/`cg_cint_ll`): pointer
width for `size_t` and `ssize_t`, and for `c_long` 32 bits on Windows and pointer width
elsewhere. Every declaration of the extern counts, so one spelled `size_t` in one module and
`int64` in another is lowered as `size_t`. An argument is truncated to the C width and a
narrower result is zero-extended (`size_t`) or sign-extended (the others) back to `int64`
(`widenCResult` / `cg_widen_cret`). On a 64-bit target nothing changes. The compiler's own
calls (`malloc` for a heap closure env, `snprintf`/`write` in the uncaught-exception
reporter) use the same `size_t`.

**Narrow integers.** Like clang, an `extern`'s `int8`/`int16`/`uint8`/`uint16`/
`bool`/`char` parameters and results are declared `signext` or `zeroext` (by the
type's signedness; `char` follows the target's C `char`, unsigned on AArch64 and
32-bit ARM outside Darwin and Windows), so the caller extends such an argument to
32 bits. Every Eskiu function returning one extends its result (`define signext
i8 @f`), since a C caller (a callback, or C calling it by name) relies on that on
AArch64 Darwin and x86-64. Two targets extend nothing, as clang: AArch64 outside Darwin
(AAPCS64 leaves the upper bits unspecified, so the receiver extends) and Windows x64,
where only a `bool` is `zeroext`. Declaring `signext` there would make Eskiu trust bits a
clang-compiled callee never set.

**Callbacks from C.** An Eskiu function handed to C as a raw function pointer
(`(*void)f`, the cast of a top-level function name to a pointer type) is called
with the C convention. When it takes or returns an aggregate by value, the cast
yields the address of a thunk `__cabi_<name>` instead of `@name`: the thunk has
the lowered C signature from the table above (with the same `sret`/`byval`
attributes), rebuilds the Eskiu-level values, calls `f`, and returns the result
the C way. A `va_list` parameter gets a thunk too, which takes the `va_list` the
way C passes it (see Variadics below) and rebuilds the Eskiu value: on x86-64
System V and AArch64 outside Darwin and Windows it copies the 24 or 32 bytes at
the address into the front of the storage, elsewhere it puts the `char*` at
offset 0 (`cVaListToEskiu` / `cg_va_from_c`). This is what C error-reporter
callbacks such as TensorFlow Lite's `void (*)(void*, const char*, va_list)` need.
A function without by-value aggregates or `va_list` parameters is passed as
itself. Eskiu code calling `f` directly is unaffected.

**C function pointer parameters.** A parameter of fn type in an `extern` (one the
program does not also define) is a C function pointer: it is declared `ptr`, not
the `{ fn, env }` closure, and each call passes the named top-level function's C
address (its `__cabi_` thunk when needed) or `null`. The type checker rejects any
other argument there, since a closure's environment cannot cross into C.

**Variadics.** A `...` parameter makes the LLVM function `isVarArg`. The built-in
`va_list` is the struct `{ ptr, ptr, ptr, i32, i32 }` (32 B, 8-aligned), a
superset of the x86-64 (24 B) and AArch64 (32 B) layouts, so one type serves
both; `va_start`/`va_end` lower to the LLVM intrinsics. `va_arg<T>` is the
LLVM `va_arg` instruction, except on AArch64 outside Darwin and Windows, whose
backend does not expand it for the AAPCS64 `va_list` `{__stack, __gr_top,
__vr_top, __gr_offs, __vr_offs}`: there the compiler emits the read as clang does
(an integer or pointer from the general-register save area while `__gr_offs` is
negative, a `float`/`double` from the FP/SIMD save area in 16-byte steps while
`__vr_offs` is, then `__stack` in 8-byte slots; `emitVaArg` / `cg_va_arg`). On
x86-64 System V LLVM expands the instruction itself for the scalars `va_arg`
reads. An `extern` parameter of type `va_list` (`vprintf`,
`vsnprintf`) is declared `ptr` and gets what C passes for its `va_list`: the
address of the storage on x86-64 System V and AArch64 outside Darwin and Windows
(an array/struct type there), and the `char*` that `llvm.va_start` stored at
offset 0 on Darwin AArch64, Windows x64 and 32-bit ARM (`evalCVaList` /
`cg_c_va_list`). Between Eskiu functions a `va_list` stays a first-class value.
Variadic arguments follow C default promotions:
integers narrower than 32 bits widen to `i32` (sign- or zero-extended), `bool`
zero-extends, and `float` widens to `double`.

---

## Function pointers, closures and interfaces

These use **fat pointers**: a two-`ptr` struct `{ ptr, ptr }`.

**Function values / closures.** A `fn(A,B)->R` value is `{ fn, env }`: a code
pointer plus an environment pointer. A lambda compiles to a function whose first
parameter is the `env*`, followed by the declared parameters; captured variables
are loaded from the env struct (one field per capture, in capture order). A bare
top-level function used as a value is wrapped by a thunk `__fnptr_<name>` with
`env == null`.

A **non-escaping** closure (only called or passed to a non-`escaping` parameter)
keeps its env on the stack (zero cost). An **escaping** closure (returned,
stored, or passed to an `escaping` parameter) heap-allocates the env;
`free_closure(f)` releases it. The choice is fixed at compile time by escape
analysis. A closure local of an `async` function lives in the coroutine frame, so
its env is on the heap; the frame frees it (see `async-design.md` §7).

**Interfaces.** An interface value is `{ data, vtable }`: a pointer to the
underlying struct plus a pointer to a per-(interface, struct) vtable constant.
The vtable is an array of function pointers, one per interface method in
declaration order. A method call loads `data` and `vtable`, indexes the vtable
by method position, and calls the function pointer with `data` as the first
argument.

**Slices.** A slice `T[]` is also a fat pointer, but `{ ptr, i64 }`: a pointer to
the first element plus an element count. `s[i]` indexes through the pointer,
`s.len` reads field 1, and the slice is passed and returned by value like any
small struct. A slice aliases its backing storage (it is built by slicing a fixed
array or a raw pointer with `a[lo..hi]`), so it neither owns nor copies the data.

---

## Name mangling

Template and generic-enum instances are monomorphized with a deterministic
mangling: in the instance type, spaces are removed and `<`, `>`, `,` become `_`.

| Source | Mangled instance |
|--------|------------------|
| `List<int>` | `List_int` |
| `Pair<int, float>` | `Pair_int_float` |
| `Option<string>` | `Option_string` |

A pointer type argument mangles its star as `_P` and an array dimension as `_AN`
(`List<int[3]*>` is `List_int_A3_P`). Struct methods are emitted as
`Struct_method`; an interface's vtable for a struct is the private constant
`Interface_vtable_Struct`, whose entries point at the `Struct_method` functions.
Non-template top-level functions keep their source names: Eskiu has no function
overloading, so no signature mangling is needed. The one exception is an operator
overload, which compiles to a function named after the operator and its operand
types (`operator +(V3 a, V3 b)` is `__op_add_V3_V3`). `extern` and
`intrinsic` declarations use their exact C symbol names: that is how Eskiu
calls into C.

---

## Async frames

An `async` function is lowered (before code generation) to a resumable state
machine: a heap-allocated **frame struct** holding the live locals plus a resume
state, a `resume` function, and a constructor that returns a `*Future<T>`. The
`Future<T>`/`FutureHdr` layout and the completion/waker handshake are specified
in [async-design.md](async-design.md); that document is the authoritative
contract for the runtime side.

---

## See also

- [../lang/grammar.md](../lang/grammar.md): the surface syntax these rules lower.
- [async-design.md](async-design.md): the `Future<T>` ABI and scheduler contract.
- [architecture.md](architecture.md): the compiler pipeline overview.
