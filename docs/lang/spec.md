# Eskiu Language Specification

**Version:** v0.12.0

---

## 1. Overview

Eskiu is a statically typed, compiled systems language. It lowers to native code through LLVM, manages memory explicitly (no garbage collector), and interoperates directly with C. A `.esk` file can also be run on the spot with `eskiuc run` (or a `#!/usr/bin/env -S eskiuc run` shebang), so the same source is both a compiled artifact and a runnable script. This document is the language reference: syntax, type system, semantics, and ABI.

Core properties:

- Statically typed with explicit type annotations
- Manual memory management, no garbage collector
- Compiles to native code via LLVM: AArch64 and x86-64 (macOS, Linux, Windows), plus cross-compilation to 32-bit ARM and 32-bit x86
- Structs with methods, monomorphic templates, and structural interfaces
- Lambdas and `fn(T)->R` function pointer types
- Multi-file programs via `import`
- Error locations reported as `file.esk:line:col: message`

The full compilation pipeline:

```sh
eskiuc file.esk -o file   # compile and link into an executable
./file                    # run
```

`eskiuc` links the program for you by invoking the system C toolchain (`$CC`,
then `cc`/`clang`/`gcc`), the same approach `rustc` and `clang` use. To stop at
the object file instead, give the output a `.o` name or pass `-c`, then link
yourself:

```sh
eskiuc file.esk -o file.o   # compile to a native object file only
clang file.o -o file        # link it yourself
```

---

## 2. Lexical Elements

A source file is bytes; a leading UTF-8 byte order mark (`EF BB BF`) is skipped.

### 2.1 Comments

Single-line comments begin with `//` and extend to the end of the line. A trailing backslash does **not** continue the comment onto the next line (unlike C). Block comments are enclosed in `/* ... */` and may span multiple lines. Comments do not nest.

```eskiu
// This is a single-line comment

/*
   This is a
   block comment.
*/
```

### 2.2 Identifiers

An identifier begins with a letter (`a`–`z`, `A`–`Z`) or underscore (`_`), followed by zero or more letters, digits (`0`–`9`), or underscores. Identifiers are case-sensitive: `point` and `Point` are distinct. Identifiers and operators are ASCII: a UTF-8 character outside a string literal, a character literal or a comment is a lexical error that names it (`non-ASCII character 'ñ' outside a string or comment`). Strings and comments may hold any UTF-8 text.

```eskiu
my_var    _internal    Count    x1
```

### 2.3 Keywords

The following identifiers are reserved and may not be used as variable or function names:

```
let  int  int8  int16  int32  int64
uint  uint8  uint16  uint32  uint64
float  double  bool  char  string  void
struct  packed  interface  fn  extern  intrinsic  import
if  else  for  while  do  in  switch  case  default  match
return  break  continue
true  false  null
alloc_with
const  volatile  static  escaping  must_use  asm
thread_create  thread_join
try  catch  finally  throw  defer  errdefer
async  await  operator
sizeof  free_closure  union  enum
```

### 2.4 Literals

**Integer decimal literals** are sequences of decimal digits:

```eskiu
0    42    1000
```

**Integer hex literals** are prefixed with `0x` followed by hexadecimal digits (`0`–`9`, `a`–`f`, `A`–`F`):

```eskiu
0xFF    0x0F    0xDEAD    0xBEEF
```

**Integer binary literals** are prefixed with `0b` (or `0B`) followed by `0` and `1` digits:

```eskiu
0b1011    0B1000_0001    0b0
```

**Digit separators.** A `_` between two digits is ignored, in every kind of numeric
literal: `1_000_000`, `0xFF_FF`, `0b1111_0000`, `1_234.5`, `1e1_0`. One more `_` may follow
the `0x`/`0b` prefix (`0x_FF`). A `_` that does not sit between two digits (`1__0`, `1_`)
is a lexical error, and so is a digit other than `0` or `1` in a binary literal. The same
forms work in a `#if` expression.

**Negative numeric literals** are written with a leading `-`:

```eskiu
-1    -42    -100
-3.14    -0.5
```

Negative literals are first-class values and can be used in any expression context, including global variable initialisers and struct field initialisers.

**Float literals** contain a decimal point. They have type `double` (f64) by default;
assigning one to a `float` (f32) variable or field coerces it down (a `double`→`float`
cast). Integer literals are `int` (i32), widening to `int64` when they exceed 32 bits.
A float literal may carry an exponent (`1e10`, `2.5e-3`), which needs at least one
digit. An integer literal that does not fit in 64 bits is a lexical error.

```eskiu
3.14    2.0    0.5
```

**String literals** are sequences of characters enclosed in double quotes. The same escape sequences are recognized in string and character literals:

| Escape | Meaning          |
|--------|------------------|
| `\n`   | Newline          |
| `\t`   | Horizontal tab   |
| `\r`   | Carriage return  |
| `\f`   | Form feed        |
| `\v`   | Vertical tab     |
| `\a`   | Alert (bell, byte 7) |
| `\b`   | Backspace (byte 8) |
| `\\`   | Backslash        |
| `\"`   | Double quote     |
| `\'`   | Single quote     |
| `\?`   | Question mark    |
| `\NNN` | Raw byte from one to three octal digits, at most `\377` (e.g. `\0` is NUL, `\012` is a newline) |
| `\xNN` | Raw byte from one or two hex digits (e.g. `\xC3` is byte `0xC3`) |

Any other escape (`\q`), an octal escape above `\377`, and `\x` with no hex digit are
errors located at the backslash. As in C, an octal escape takes at most three digits
(`"\1234"` is `S` followed by `4`), and `\0` followed by a non-octal character is NUL.

```eskiu
"Hello, world!\n"
"path\\to\\file"
"column\theader"
```

**Character literals** are a single character or escape sequence enclosed in single quotes:

```eskiu
'a'    '\n'    '\\'
```

**Boolean literals** are the keywords `true` and `false`.

**Null literal** is the keyword `null`, used for null pointer values.

---

## 3. Types

### 3.1 Primitive Types

| Type     | LLVM IR  | Width   | Notes                              |
|----------|----------|---------|------------------------------------|
| `int`    | `i32`    | 32 bits | Signed; alias for `int32`          |
| `int8`   | `i8`     | 8 bits  | Signed                             |
| `int16`  | `i16`    | 16 bits | Signed                             |
| `int32`  | `i32`    | 32 bits | Signed                             |
| `int64`  | `i64`    | 64 bits | Signed                             |
| `uint`   | `i32`    | 32 bits | Unsigned; alias for `uint32`       |
| `uint8`  | `i8`     | 8 bits  | Unsigned                           |
| `uint16` | `i16`    | 16 bits | Unsigned                           |
| `uint32` | `i32`    | 32 bits | Unsigned                           |
| `uint64` | `i64`    | 64 bits | Unsigned                           |
| `float`  | `float`  | 32 bits | IEEE 754 single-precision          |
| `double` | `double` | 64 bits | IEEE 754 double-precision          |
| `bool`   | `i1`     | 1 bit   | `true` or `false`                  |
| `char`   | `i8`     | 8 bits  | Unsigned; single byte              |
| `string` | `ptr`    | pointer | Immutable C-string literal         |
| `void`   | `void`   | n/a     | No value; valid only as return type|

Signedness is tracked by the compiler for correct arithmetic and comparison codegen. Signed and unsigned variants of the same width share the same LLVM integer type (e.g., `int8` and `uint8` are both `i8`).

Arithmetic follows C's integer promotions: an operand narrower than `int` (`bool`, `char`, `int8`, `int16`, `uint8`, `uint16`) is converted to `int` before an arithmetic, bitwise, shift, or comparison operator is applied. So `(uint8)200 + (uint8)100` is `300`, `(uint8)200 > (int8)-1` is true, and `true + true` is `2`. Storing the result back into a narrow variable truncates it, as in C. Converting any integer, floating, or pointer value to `bool` yields `value != 0`.

After promotion, two operands of different types meet under C's usual arithmetic conversions: the wider operand's type wins, and at equal width an unsigned operand makes the result unsigned, so `(int32)-1 + (uint32)5` is the `uint32` `4` and `(int32)-1 < (uint32)5` is false. A shift has the type of its promoted left operand, and unary `-` and `~` promote a narrow operand to `int` first. The two arms of a ternary meet the same way (two different narrow arms give an `int`). An integer literal that does not fit in `int` has type `int64`, so `3000000000 * 2` is `6000000000`. Constant initializers fold with exactly these rules.

### 3.2 Pointer Types

A pointer type is written with a leading `*`:

```eskiu
*int       // pointer to int
*uint8     // pointer to uint8
**char     // pointer to pointer to char
```

Both leading-star (`*T`) and trailing-star (`T*`) syntax are accepted. The canonical form in this document is `*T`.

```eskiu
let ptr: *int = null;
let buf: *uint8 = null;
```

A pointer converts implicitly only to a pointer to the same type (adding `const` is
fine). `null` converts to any pointer, a `*void` converts to and from any pointer, and
the byte pointers `string`, `*char`, `*int8` and `*uint8` interconvert. Any other change
of pointee, such as `*int` to `*Big` or `**int` to `*int`, needs an explicit cast
(`(*Big)p`). A `*void` points at no type, so it cannot be dereferenced (`*p` is a compile
error); cast it to a typed pointer first.

#### Checked nullable pointers (`?*T`)

A plain `*T` may hold `null` (as in C), and dereferencing a null one is undefined. For
opt-in null safety, write `?*T`, a **checked nullable pointer**. The compiler will not let
you dereference, index, or take a member of a `?*T` until you have proven it non-null with
a null-check; inside `if (x != null) { ... }` the pointer is treated as non-null:

```eskiu
?*int q = maybe();
*q;                        // error: q may be null; check it first
if (q != null) {
    printf("%d\n", *q);    // ok: narrowed to non-null here
}
```

A non-null `*T` converts to `?*T` implicitly (widening); going the other way (`?*T` to
`*T`) drops the check and so requires narrowing (or an explicit cast). `?*T` has the same
representation as `*T` (a bare pointer); the checking is entirely at compile time. The
`?` marks a pointer type only: `?int` is an error. The `?` applies to the whole type it starts, so `?*T*` is a nullable pointer to `*T`. A `?` after a leading `*` and before another one makes the inner pointer nullable: `*?*T` is a pointer to a nullable pointer to `T` (an out-parameter that may receive `null`). Dereferencing it once gives a `?*T`, which must be checked before it is dereferenced again, and it does not convert to `**T`, which would drop that check (`&p` of a `?*T` variable `p` is a `*?*T`).

A null-check narrows in these forms: the then-branch of `if (x != null)` (and the else of
`if (x == null)`), `if (x)`, `!(x == null)`, the right operand of `x != null && ...` and of
`x == null || ...`, either arm of a `?:` on such a condition, the body of
`while (x != null)` or `for (...; x != null; ...)`, and the rest of a block after an early
exit such as `if (x == null) { return 0; }` (unless the branch that falls through assigns
`x`). While narrowed, `x` can be passed or assigned
where a `*T` is expected. Narrowing applies to the variable itself: assigning to it (other
than storing an address `&y`), taking its address, or reassigning it anywhere in an
enclosing loop ends it, and a shadowing declaration of the same name is not narrowed. A
variable whose address has been taken (`&x`, before the check or anywhere in an enclosing
loop) is not narrowed, since a write through that pointer can store null behind the check;
for a global the address counts anywhere in the program. A global's narrowing also ends at
every call and `await`, and at `alloc_with` (a call to the alloc method), `thread_create`
and `thread_join`, since the code they run may assign it: a test of a global earlier in a
condition does not hold after a later operand that calls (`gp != null && f() && gp.v`),
nor in a branch or ternary arm guarded by such a condition, nor after an early-exit guard
whose condition or fall-through branch calls something. A user operator (`a + b` on a
struct with an `operator +`) is a call too. A lambda body never sees a global's narrowing
from where the lambda is written, since it runs later. A `static` local is one cell shared
by every call and closure, so it follows the rule for globals.

### 3.3 Array Types

Fixed-size arrays use the form `T[N]` where `N` is a positive integer constant expression, as in C: numbers, `enum` members and `const int`s (see §4.6), casts to an integer type, the integer operators including comparisons and `?:` (`int[K << 1]`, `int[K > 2 ? 4 : 1]`), and `sizeof(T)` (`uint8[sizeof(Header)]`), whose value is the target's layout size. An array of an array alias puts the new dimension outermost: with `type Row = int[3]`, `Row[2]` is `int[2][3]`. Array types are supported both as struct fields and as local variables:

```eskiu
struct QRBuffer {
    uint8[858] left;
    uint8[858] right;
    int length;
}

int main() {
    int[16] scratch;   // local fixed-size array
    return 0;
}
```

When a leading `*` meets a trailing `[N]`, the **array binds outermost**: `*T[N]` is an
*array of N pointers* (each element a `*T`), i.e. it reads as `(*T)[N]`. For example
`*Node[7]` is seven `Node` pointers, so `*p` of one is an error (index it first, `*p[0]`).
There is no source spelling for a pointer to a whole array (`T[N]*` does not parse, and
`&arr` is not a `*T[N]`); point at the first element instead, `*Node p = &arr[0];`, and
index through it (`p[i]`), or name the array with an alias: with `type Row = int[3]`, a
`*Row` points at a whole `int[3]` (`(*p)[2]`).

The same rule applies to a function type: `fn(int)->int[2]` is an *array of 2 function
values*, not a function returning an array (a function cannot return an array), so
`fs[1](41)` calls the second element.

`uint8[858]` lowers to `[858 x i8]` in LLVM IR.

An array may be initialized with a brace list `{ e0, e1, ... }`. As in C, listing fewer
elements than the size zero-fills the rest, and `{}` zero-fills the whole array; listing
more than the size is an error.

```eskiu
int[3] a = {10, 20, 30};   // a = 10, 20, 30
int[4] b = {7, 8};         // b = 7, 8, 0, 0
int[3] c = {};             // c = 0, 0, 0
```

**Multidimensional arrays** chain the suffix: `T[N][M]` is `N` arrays of `M` elements,
following C order, so the **leftmost bracket is the outer dimension**. Indexing peels one
dimension at a time (`a[i]` is a row of type `T[M]`, `a[i][j]` is a `T`), and each index
is bounds-checked against its own dimension. A nested brace list initializes it, with the
same zero-fill rule at every level:

```eskiu
int[2][3] a = { {1, 2, 3}, {4, 5, 6} };   // 2 rows of 3
int[2][2] b = { {7, 8} };                 // second row zero-filled: {7,8},{0,0}
a[1][2];                                    // 6  (row 1, column 2)
```

`int[2][3]` lowers to `[2 x [3 x i32]]` in LLVM IR.

### 3.3.1 Slice Types

A **slice** `T[]` (empty brackets) is a view into a contiguous run of `T`: a fat pointer
carrying both a data pointer and a length. Unlike a fixed array, a slice's length travels
with it, so a function that takes a `T[]` never needs a separate count argument.

Create a slice by **slicing** a fixed array *or a raw pointer* with a half-open range
(reusing the `..` operator): `a[lo..hi]` is a view of elements `lo` through `hi-1`.
Slicing a `*T` (for example a heap buffer from `alloc<T>(n)`) yields a `T[]` over that
memory, so slices are not limited to fixed arrays. The slice **aliases** the backing
storage; writing through it writes to the array or buffer.

```eskiu
int[6] a = {10, 20, 30, 40, 50, 60};

int[] mid = a[1..4];     // view of {20, 30, 40}
mid[1] = 99;             // writes through: a[2] is now 99
```

A slice supports indexing (`s[i]`, read and write), its length via `s.len` (an `int64`),
and iteration with `for (x in s)`. Pass it by value: the fat pointer (data + length) is
copied, but it still refers to the same backing storage.

```eskiu
int sum(int[] s) {
    int total = 0;
    for (x in s) { total = total + x; }   // length-driven, no count argument
    return total;
}

sum(a[0..6]);            // the whole array as a slice
```

`int[]` lowers to `{ ptr, i64 }` in LLVM IR. By default an out-of-range slice or array
index is undefined, as in C; compile with `--safe` to bounds-check every index at runtime
(an out-of-range access aborts instead of corrupting memory), or with `--safe=bounds` for
this check alone. `--safe` is opt-in, so release builds carry no overhead.

### 3.4 Struct Types

A struct type is named by its declaration (see §8). Variables of struct type are declared using the struct name as the type annotation:

```eskiu
let p: Point;
let r: Rect;
```

### 3.5 Template Types

Template structs and functions are parameterized by one or more type variables. Instantiation is lazy and monomorphic: the compiler generates one concrete definition per unique set of type arguments.

```eskiu
let r: Result<int, string>;
let items: List<float>;
```

`Result<int, string>` lowers to `%Result_int_string` in LLVM IR.

### 3.6 Interface Types

Interface types are structural: any struct that provides all required methods satisfies the interface without an explicit declaration. An interface value is a fat pointer `{data_ptr, vtable_ptr}`; the compiler builds one from a pointer to a conforming struct (`&x`) wherever an interface is expected (see §9.4). Passing the struct itself by value is an error. Each required method must match the interface's signature (return type and parameter types after the receiver; a type spelled with the interface's own name stands for the implementing type).

```eskiu
interface Drawable { void draw(); }
// any struct with a draw() method satisfies Drawable
```

### 3.7 Function Pointer Types

A function pointer type is written using the `fn` keyword:

```eskiu
fn(int)->int          // function taking one int, returning int
fn(int, int)->bool    // function taking two ints, returning bool
fn()->void            // function taking no arguments, returning void
```

> **`fn` names a type, never a definition.** Unlike Rust, Swift, or Kotlin, Eskiu does
> not use `fn` to *define* functions. A function is defined C-style, with the return type
> before the name: `int add(int a, int b) { ... }` (see §6.1). The `fn(...)->R` form appears
> only where a *type* is expected: a variable's type, a struct field, or a parameter. There
> is no `fn name(...)` definition syntax.

Function pointer types can be used anywhere a type annotation is expected: variable declarations, struct fields, and function parameters.

```eskiu
let callback: fn(int)->int = int(int x) { return x * 2; };
int apply(fn(int)->int f, int x) { return f(x); }
```

A function *value* is assignable to an `fn(...)` type only when the signatures match
exactly: a function's parameter and return registers are fixed by its calling convention,
so there is no implicit conversion between function types. Assigning, say, an
`fn(int)->int` value to an `fn(int)->float` is a compile error. (A lambda *literal* is the
one exception: when its header return type differs from the target `fn(...)->R`, the
return type is taken from `R` and the body's value is coerced, so `fn(int)->float =
int(int x){ ... }` is accepted.)

### 3.8 Type Casting

An explicit cast is written as `(TYPE)expr`. The expression is converted to the named type at the point of the cast.

```eskiu
double x = 3.14;
int n = (int)x;        // truncates to 3
uint8 b = (uint8)n;
float f = (float)n;
```

### 3.9 Numeric Conversions

Numeric conversions in assignment, initialization, `return`, and call arguments follow
C, with one exception. Implicit (no cast needed):

- **Widening**: `int8` to `int`, `int` to `int64`, `int` to `float`, and so on.
- **Integer-width narrowing**: `int64` to `int`, `int` to `uint8`. The value is truncated
  (as in C); write a cast when the truncation is not what you want.
- **Float-width narrowing**: `double` to `float`.

The one conversion that requires an explicit cast:

- **Floating-point to integer** (`double`/`float` to any integer). It drops the
  fractional part, so it must be written out: `int n = (int)3.9;` gives `3`; `int n =
  3.9;` is a compile error. A value outside the target's range saturates to the nearest
  limit and NaN converts to 0: `(uint8)300.5` is 255, `(uint8)-1.0` is 0, `(int)1e20`
  is 2147483647 (in C these are undefined behavior).

Statically-known mistakes are also compile errors:

- An **integer literal that does not fit** its target type: `int8 x = 300;` (300 is
  outside `int8`'s range). A literal that fits is fine: `uint8 c = 255;`.
- **Integer division or remainder by zero**, or of the most negative value by `-1`, when
  the operands are constant: a literal (`x / 0`), a `const` name, a `sizeof` (§5.8),
  a cast (`x / (int)0.5`) or an expression of those. `M / -1` with `const int M =
  -2147483647 - 1` does not fit an `int`.
- A **floating constant converted to an integer type that cannot hold it**, written as a
  literal or reached through `const` names and arithmetic: `(int)1e10`, `(int)D` with
  `const double D = 1e10`, `(uint32)(0.0 - 1.0)`.

A constant shift count out of range and a constant array index out of bounds are errors
by the same folding (`a << sizeof(int64) * 8`, `a[sizeof(int)]` on an `int[4]`).

```eskiu
int64 big = strlen(s);   // ok: no cast needed for the length
int   n   = strlen(s);   // ok: int64 to int truncates, as in C
int   x   = (int)3.9;    // ok: explicit
int   y   = 3.9;         // error: floating-point to integer needs a cast
int8  z   = 300;         // error: literal out of range for int8
```

---

## 4. Variables

### 4.1 let-style Declaration

```eskiu
let x: int = 5;
let name: string = "Eskiu";
let ptr: *int = null;
let pt: Point;
```

### 4.2 C-style Declaration

```eskiu
int x = 5;
string name = "Eskiu";
*int ptr = null;
```

Both forms are equivalent. The type annotation is required in both; type inference is not supported.

A variable is visible from its declaration to the end of the enclosing block. A declaration in a nested block may **shadow** an outer variable of the same name; the outer variable is unchanged and visible again after the inner block ends. Declaring the same name twice in one scope is an error, and so are these other duplicates: two parameters with one name, a local in a function's outermost block that reuses a parameter's name (the parameters and that block share one scope, as in C), two fields (or a field and a method) with one name in a struct, a repeated enum member, a second `default:` in a `switch`, a global and a function (or a struct and a function) with one name, and a function prototype whose signature differs from its definition. A prototype that matches its definition, and an `extern` declaration next to the variable's definition, are allowed.

### 4.3 Pointer Variables

```eskiu
let p: *int = null;
if (p != null) {
    int val = *p;
}
```

### 4.4 Struct Variables

```eskiu
let pt: Point;
pt.x = 1.5;
pt.y = 2.5;
```

### 4.5 Volatile Variables

The `volatile` qualifier prevents the compiler (via LLVM) from optimising away loads and stores to a variable. It is required for memory-mapped I/O (MMIO) registers whose value may change or have side effects outside the program's control.

```eskiu
volatile let uart: *uint8 = (uint8*) 0x3F8;
*uart = 'A';   // store is always emitted, not eliminated by optimiser
```

`volatile` applies to every load and store of the variable itself and of any place reached from it by `*`, `[]` or `.` (`*uart`, `uart[i]`, `dev.ctrl`), including `++`/`--`, compound assignment and the storage word of a bitfield (`dev.mode = 5` reads and writes the word with volatile accesses), for locals and globals. The initializing store of a volatile variable is volatile too. It has no effect on variables that are never accessed through a pointer, but the canonical use is MMIO pointer variables as shown above.

### 4.6 Constants (`const`)

The `const` qualifier declares an immutable, typed binding. It prefixes either declaration form, must be initialised, and may not be reassigned:

```eskiu
const int MAX = 100;
const let step: int = 5;

MAX = 200;   // error: cannot assign to read-only location 'MAX'
```

A `const` integer can also be used as a **fixed-size array dimension**, in struct fields and in local variables:

```eskiu
const int CAP = 4;

struct Ring { int[CAP] slots; }   // CAP resolves at compile time

int main() {
    int[CAP] xs;                  // local array sized by a constant
    xs[0] = 1;
    return sizeof(Ring);          // 16
}
```

An array dimension is an integer constant expression: literals, `enum` members and `const` ints, combined with the integer operators, `?:`, parentheses and casts to an integer type, which truncate as in C (`int[(uint8)258]` has 2 elements, `int[CAP * 2]` 8). It must be positive, and the array may not be larger than the largest object of the target (2^61-1 bytes with 64-bit pointers, 2^32-1 with 32-bit ones; "array is too large", as in C). `const` bindings are block-scoped like any other variable.

**`const` works on any type** (string, struct, pointer, scalar). Immutability covers both rebinding the variable and mutating a field or element of a `const` value:

```eskiu
const string name = "Eskiu";   // any type may be const
const let p: Point = Point { x: 1.0, y: 2.0 };
p.x = 5.0;                      // error: cannot assign to read-only location 'p'
```

### Pointer constness

For pointers, `const` distinguishes *what* is read-only, exactly as in C:

| Spelling | Meaning | Pointer rebindable? | Pointee writable? |
|---|---|---|---|
| `int*`         | ordinary pointer        | yes | yes |
| `const int*`   | pointer to const int    | yes | **no** |
| `int* const`   | const pointer to int    | **no** | yes |
| `const int* const` | const pointer to const int | **no** | **no** |

```eskiu
int sum(const int* p, int n) {  // a read-only view of the caller's data
    int s = 0;
    for (i in 0..n) { s = s + p[i]; }   // reads through the const pointer are fine
    return s;
}

const int* r = &v;
r = &w;     // allowed: the pointer is rebindable
*r = 10;    // error: cannot assign to read-only location 'r'  (pointee is const)

int* const c = &v;
c = &w;     // error: cannot assign to read-only location 'c'  (binding is const)
*c = 10;    // allowed: the pointee is writable
```

Const-correctness is enforced on conversions: adding const (`int*` → `const int*`) is always allowed, but any conversion that would **drop** a const qualifier (in an initializer, assignment, call argument, or return) is a compile error. `const` has no ABI effect; it is stripped before code generation. It applies uniformly to locals, parameters, struct fields and return types.

A method call `x.m()` passes `&x` as `self`, so on a `const` value (or through a `const T*`) it is allowed only when the method declares a read-only receiver, `int P_get(const P* self)`. A method whose `self` is a plain `*P` may write through it and is rejected there.

### 4.7 Static Locals (`static`)

The `static` qualifier gives a **local** variable a single instance that persists across calls, exactly as in C. Its storage lives for the whole program, not the enclosing call, so it retains its value between invocations:

```eskiu
int next() {
    static int c = 0;   // initialised once, at load time
    c = c + 1;
    return c;
}

next();   // 1
next();   // 2
next();   // 3
```

A `static` local's initializer must be a **compile-time constant**; a runtime expression is rejected. An uninitialised `static` local is zero-initialised. Two `static` locals in different functions never alias, even if they share a name. `static` on a global is rejected, since a global already has static storage. A closure that uses a `static` local refers to that one cell, like a global, rather than capturing a copy.

The same constant rule applies to a **global** variable's initializer, as in C: it may be a literal, an enum member, a top-level `const`, `sizeof`, the address of a global (`&g`), a non-capturing lambda, any unary, binary, ternary or cast expression over those (`int C = 3 + 1;`, `double q = 1.0 / 4.0;`), or an array or struct literal built from them. A top-level function name is a constant too (`Op g = add;`, as in C): it decays to a closure with no environment. A call or a read of a non-`const` variable is a compile error ("initializer of global 'X' is not a compile-time constant"), since no code runs before `main` to compute it.

---

## 5. Operators

### 5.1 Arithmetic

| Operator | Description        |
|----------|--------------------|
| `a + b`  | Addition           |
| `a - b`  | Subtraction        |
| `a * b`  | Multiplication     |
| `a / b`  | Division           |
| `a % b`  | Modulo (remainder) |

Integer division truncates toward zero. Float operands use IEEE 754 semantics.

Integer arithmetic has defined results at run time, the same at every optimization level
(unlike C, where these cases are undefined behavior):

- `+`, `-`, `*`, unary `-` and `++`/`--` wrap around in two's complement on overflow, for
  signed types as for unsigned ones: `int x = 2147483647; x + 1` is `-2147483648`. Under
  `--safe` a signed `+`, `-` or `*` on `int` or `int64` that overflows traps instead
  (Zig's ReleaseSafe; unsigned and narrow arithmetic still wraps).
- A zero divisor in `/` or `%` stops the program with a trap (the process ends with a
  signal, SIGTRAP or SIGILL depending on the CPU), as `--safe` bounds checks do. A divisor
  the compiler can see is constant (`x / 0`) is a compile error instead (3.9).
- The most negative value divided by `-1` wraps to itself, and its remainder is 0:
  `-2147483648 / -1` is `-2147483648`.

### 5.2 Bitwise

| Operator | Description  |
|----------|--------------|
| `a & b`  | Bitwise AND  |
| `a \| b` | Bitwise OR   |
| `a ^ b`  | Bitwise XOR  |
| `~a`     | Bitwise NOT  |
| `a << b` | Left shift   |
| `a >> b` | Right shift  |

Right shift on signed types is arithmetic (sign-extended).

The shift count is taken modulo the width of the (promoted) value shifted: for an `int` it
is `count & 31`, for an `int64` `count & 63`, so `1 << 32` with a run-time count of 32 is
`1` and a negative count uses its low bits. This is what x86 and AArch64 shift instructions
do, so it costs nothing. A constant count out of range is a compile error (3.9).

### 5.3 Comparison

| Operator | Description           |
|----------|-----------------------|
| `a == b` | Equal                 |
| `a != b` | Not equal             |
| `a < b`  | Less than             |
| `a > b`  | Greater than          |
| `a <= b` | Less than or equal    |
| `a >= b` | Greater than or equal |

Comparison works on integer types, floating-point types, and pointer types. The result is always `bool`.

### 5.4 Logical

| Operator   | Description  |
|------------|--------------|
| `a && b`   | Logical AND  |
| `a \|\| b` | Logical OR   |
| `!a`       | Logical NOT  |

Short-circuit evaluation applies: in `a && b`, `b` is not evaluated if `a` is false; in `a || b`, `b` is not evaluated if `a` is true.

### 5.5 Assignment

| Operator  | Description                  |
|-----------|------------------------------|
| `x = e`   | Assign                       |
| `x += e`  | Add and assign               |
| `x -= e`  | Subtract and assign          |
| `x *= e`  | Multiply and assign          |
| `x /= e`  | Divide and assign            |
| `x %= e`  | Modulo and assign            |
| `x &= e`  | Bitwise AND and assign       |
| `x \|= e` | Bitwise OR and assign        |
| `x ^= e`  | Bitwise XOR and assign       |
| `x <<= e` | Left-shift and assign        |
| `x >>= e` | Right-shift and assign       |

`x op= e` is equivalent to `x = x op e`, except that `x` is evaluated only once (see below).

The left-hand side must be an lvalue: a named variable, a pointer dereference (`*ptr = value`), or a field access. A field or element of a temporary (`mk().x`, `arr()[0]`) is not an lvalue: it can be read, but not assigned, have its address taken, or (an array) be sliced. Assigning through a dereferenced pointer parameter works correctly. `*ptr = value` stores through the pointer as expected.

Evaluation order: the address of the left-hand side is computed first (its index, pointer and call subexpressions run), then the right-hand side, then the store. So `a[f()] = g()` calls `f` before `g`, and `getp().x = g()` calls `getp` first. A compound assignment `x op= e` evaluates the address of `x` once, in the same order.

### 5.5.1 Increment and Decrement

`++` and `--` add or subtract one from an integer or pointer lvalue, in place. Both prefix
and postfix forms are supported: `++x`/`--x` yield the new value, `x++`/`x--` yield the old
value. On a pointer, the step is one element (like pointer arithmetic).

```eskiu
for (int i = 0; i < n; i++) { ... }   // postfix, value discarded
int a = i++;   // a = old i, then i incremented
int b = ++j;   // j incremented, then b = new j
```

The operand must be a modifiable lvalue (a `const` or a non-lvalue like `5++` is an error).

### 5.6 Address-of and Dereference

| Operator | Description                                  |
|----------|----------------------------------------------|
| `&x`     | Address of `x`; yields `*T` where `x: T`    |
| `*ptr`   | Dereference `ptr`; yields `T` where `ptr: *T`|

For a local variable, `&x` is the address of its stack slot.

```eskiu
int val = 42;
*int ptr = &val;
*ptr = 100;       // val is now 100
```

### 5.7 Pointer Arithmetic

Adding or subtracting an integer `n` from a pointer of type `*T` advances the pointer by `n * sizeof(T)` bytes (typed GEP). This matches C semantics: `p + 1` moves to the next element, not the next byte.

```eskiu
*int  pi = alloc<int>(8);
*int  p2 = pi + 1;   // 4 bytes forward, points to element 1
```

The exceptions are `*void` and `*char`, which always use byte-level stride (1 byte per step) to preserve C interop semantics:

```eskiu
*uint8 buf = alloc<uint8>(1024);
*uint8 mid = buf + 512;    // 512 bytes into the buffer
*uint8 back = mid - 512;   // back to start
```

Subtracting two pointers of the same type gives an `int64` count of **elements** between them, as in C: `(pi + 3) - pi` is `3` for a `*int`, not `12`.

### 5.8 sizeof Expression

`sizeof(T)` is a compile-time constant expression that evaluates to the size of type `T` in bytes as an `int64`. It works for all Eskiu types, including structs and unions.

```eskiu
sizeof(int)    // 4
sizeof(int64)  // 8
sizeof(float)  // 4
sizeof(double) // 8
sizeof(Grid)   // 12  (3 float fields)
```

`sizeof` is resolved entirely at compile time and produces no runtime code. As in C, `sizeof(x)` where `x` names a variable (or a parameter or global) gives the size of that variable's type; a name that is neither a type nor a variable is an error.

The operand may also be an expression: `sizeof(*p)`, `sizeof(a[0])`, `sizeof(s.field)` give the size of the expression's type, and the expression is not evaluated (`sizeof(f())` does not call `f`). The operand is read as a type when it is a bare name or a spelling built over a primitive or an already declared type (`sizeof(*Node)`, `sizeof(int[4])`); otherwise it is an expression.

The type checker folds `sizeof` of a scalar, a pointer, a closure, an interface, a slice, a fixed array, a sum type, a generic instance, and a struct or union of those (bitfields and `#pragma pack(N)` included), using the target's layout (`--target`), so the constant checks below (a zero divisor, an array index out of bounds, a duplicate `case`) and `enum` member values see it. A union declared under `#pragma pack(N)` folds too.

### 5.8.1 dbg Expression

`dbg(e)` evaluates `e` once, prints its source text and value to stderr, and yields the value, so it can wrap any subexpression:

```eskiu
int m = dbg(n + 1) * 2;     // [dbg 12:13] n + 1 = 21
dbg(p);                     // [dbg 14:5] p = P{x: 1, y: 2.5, s: "hi"}
```

The line and column are those of `dbg`. Integers, `bool`, `char` (`'z'`), floating point (`%g`), `string` (quoted, `null` unquoted) and pointers (`%p`) print as values; a fixed array prints its first 16 elements, a struct (a generic instance by its mangled name, `Box_int{...}`) its fields by name, nested up to 4 levels; a plain enum prints as `Color(5)`; any other type (a slice, a closure, a union, a sum type, a bitfield struct) prints as `<T>`. The argument may not be a `void` call. `dbg` is not a reserved word: a variable or function named `dbg` hides the builtin. It prints through the C library (`fprintf` to stderr), so a `--freestanding` program cannot use it.

### 5.9 Conditional (ternary)

`cond ? a : b` evaluates `cond`, then evaluates and yields **exactly one** of the two
arms (so side effects in the unused arm never run). The condition may be a bool,
integer, or pointer (non-zero / non-null is true). The two arms must share a common
type: identical types pass through, two numerics promote to the wider (C-style, e.g.
`int` and `double` yield `double`), and otherwise the arms must be mutually assignable.
When the value goes to an interface (a declaration, assignment, return or argument of
that type), each arm converts to it on its own, so the arms may point at different
structs that satisfy it: `Shape s = round ? &c : &sq;`.
The operator is right-associative, so `a ? b : c ? d : e` parses as `a ? b : (c ? d : e)`.

```eskiu
int m = a > b ? a : b;                       // max
char g = s >= 90 ? 'A' : s >= 80 ? 'B' : 'C';  // right-associative chain
double d = flag ? 1 : 2.5;                   // arms promote to double
```

Eskiu also uses `?` as the postfix Result-propagation operator (§10.5). The two are
disambiguated by the following `:`: a `?` with a matching `:` at the same bracket
nesting is a ternary, otherwise it is propagation. To propagate inside a ternary arm,
parenthesize it: `cond ? (may_fail()?) : fallback`.

### 5.10 Operator Precedence

Listed from lowest precedence (loosest binding) to highest (tightest binding):

| Level | Operators                                    | Associativity |
|-------|----------------------------------------------|---------------|
| 1     | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | Right to left |
| 2     | `?:` (ternary)                               | Right to left |
| 3     | `\|\|`                                       | Left to right |
| 4     | `&&`                                         | Left to right |
| 5     | `\|` (bitwise)                               | Left to right |
| 6     | `^`                                          | Left to right |
| 7     | `&` (bitwise)                                | Left to right |
| 8     | `==` `!=`                                    | Left to right |
| 9     | `<` `>` `<=` `>=`                            | Left to right |
| 10    | `<<` `>>`                                    | Left to right |
| 11    | `+` `-`                                      | Left to right |
| 12    | `*` `/` `%`                                  | Left to right |
| 13    | Unary `!` `-` `+` `~` `&` `*` `(TYPE)`      | Right to left |
| 14    | `()` `[]` `.` `?` (postfix)                  | Left to right |

Use parentheses to override precedence explicitly.

---

## 6. Functions

### 6.1 Regular Functions

```eskiu
int add(int a, int b) {
    return a + b;
}

int get_magic() {
    return 42;
}
```

Parameters are passed by value. The return type is declared before the function name. This is the only function-definition form; Eskiu has no `fn name(...)` definition syntax (the `fn` keyword names a function-pointer *type*, see §3.7).

**Declaration order is irrelevant.** A function may call any other function regardless of where it appears in the file, so call-before-definition and mutual recursion both work without ceremony. A body-less *forward declaration* is also permitted (and optional):

```eskiu
int is_odd(int n);                              // forward declaration

int is_even(int n) {
    if (n == 0) { return 1; }
    return is_odd(n - 1);                       // defined below, fine
}

int is_odd(int n) {
    if (n == 0) { return 0; }
    return is_even(n - 1);
}
```

**A non-void function must return a value on every path.** Letting control fall
off the end of the body is a compile error (`missing return in non-void
function`). There is no implicit zero return, and the last expression in the body
is not treated as the result. A function whose body provably cannot fall through
satisfies the rule without a trailing `return`: for example one that ends in an
`if`/`else` where both branches return, an exhaustive `switch`/`match` where every
arm returns, or an infinite `while (1)` loop with no `break`.

```eskiu
int classify(int x) {
    if (x < 0) { return 0; }
    else       { return 1; }
}                              // ok: every path returns

int bad(int x) {
    if (x < 0) { return 0; }
}                              // error: missing return (x >= 0 falls through)
```

### 6.1.1 must_use

Prefixing a function with `must_use` makes discarding its result a compile error. It
catches the "called for a value, then dropped it" bug, most importantly a leaked
allocation. The standard library marks `alloc` this way:

```eskiu
must_use *T dup<T>(T* p) { ... }

dup(&x);              // error: result of 'dup' must be used (it is marked must_use)
*int y = dup(&x);     // ok (x is an int)

alloc<uint8>(64);     // error: the allocation is leaked (alloc is must_use)
```

A call is "used" if its result is assigned, passed as an argument, returned, or otherwise
consumed; only a bare call statement discards it.

### 6.1.2 deprecated

`deprecated("message")` before a function declaration marks the function as on its way
out. It keeps working, and each use outside an imported module prints a warning with the
message:

```eskiu
int Rect_area(const Rect* self) { return self.w * self.h; }
deprecated("use Rect_area") int rect_area(const Rect* r) { return Rect_area(r); }

int a = rect_area(&r);   // warning: 'rect_area' is deprecated: use Rect_area
```

A call, a dot-call, a generic call and the function used as a value all count as uses.
`-Wno-deprecated` turns the warnings off. `deprecated` is not a keyword: it is recognized
only as `deprecated("...")` at the start of a top-level declaration, so a variable or
function named `deprecated` is still valid, and it applies only to functions. The release
policy that uses it is in [stability.md](stability.md).

### 6.1.3 Default Parameter Values

A parameter may give a default value, used when a call leaves that argument out:

```eskiu
void connect(string host, int port = 443, bool verbose = false) { ... }

connect("example.com");            // port 443, verbose false
connect("example.com", 8080);      // verbose false
```

Every parameter after one with a default needs a default too, and a variadic function takes none. A default is a literal (`"none"`, `'x'`, `true`, `null`, `-1.5`) an integer constant expression over literals, enum members and top-level `const` integers (`BASE + 43`), or a struct literal whose values are any of these, nested struct literals included (`V3 up = V3{0.0, 1.0, 0.0}`, `Seg s = Seg{a: V3{}, n: N * 2}`); fields the literal leaves out take the struct's field defaults, and each call gets its own copy. It is folded and checked against the parameter's type where the function is declared, so a local of the caller with the same name does not change it. Defaults may be written on the prototype or on the definition, not on both. They apply to direct calls, to generic functions, `async` functions and inline methods of non-generic structs (`s.add()`), and to a top-level `T_method(self, ...)` called with dot syntax, a generic one included (`c.get()` on a `Cell<int>`). Calls through a function value (`fn(...)->R`) pass every argument. `extern` and `intrinsic` prototypes, interface methods and lambdas take no defaults.

### 6.2 Void Functions

```eskiu
void log_event(string msg) {
    printf("%s\n", msg);
}
```

A `void` function may use `return;` with no operand or allow control to fall off the end of the body. It may also `return f();` where `f` is itself `void` (the call runs, nothing is returned).

A call to a `void` function has no value: it can be a statement, the operand of `return` in a `void` function, or both arms of a `?:` used as a statement (`c ? f() : g();`), but not an operand of an operator (`&&`, `||`, a comparison, arithmetic), a condition, an initializer, or an argument (also not one passed through `...`, as in `printf("%d", f())`).

The one function that may not be `void` is `main`: its return value is the process exit
code, so it must return `int` (`int main()` or `int main(int argc, string* argv)`). A
`void main()` is a compile error.

**Scripts.** A file may instead have statements at the top level, mixed with declarations. It is then a script: the compiler gives it `int main(int __argc, string* __argv)`, whose body is those statements in source order. The arguments are in the globals `int argc` and `string* argv`. A top-level variable of a script is a global that the functions see; one with an initializer starts at zero and is assigned at its place in that order, so its initializer may be any expression (a call, `argv[1]`), not only a constant. A `const` variable and an array-literal initializer keep the global rules (a constant). A statement cannot name a global declared after it, as a function cannot. A `return n;` ends the script with exit code `n`; reaching the end returns 0.

```eskiu
extern int printf(string fmt, ...);
int total = 0;
void add(int n) { total += n; }
add(3);
add(4);
string who = argc > 1 ? argv[1] : "world";
printf("%d %s\n", total, who);      // 7 world
```

A top-level line is a statement when no declaration can start with its tokens: a statement keyword (`if`, `for`, `while`, `return`, ...), `{`, a literal or prefix operator, a call `f(...)` or `f<int>(...)`, a comparison `x < y`, or an assignment, member or postfix use of a name (`x = 1`, `p.x = 2`, `arr[0] = 3`, `*p = 4`); `Point[2] ps;` and `*Node p;` still declare. A file with top-level statements may not also define `main`, and an imported file may not have them.

### 6.3 Variadic Functions

The ellipsis `...` marks a variadic parameter list; at least one fixed parameter
must precede it. It is valid in both `extern` declarations and user functions:

```eskiu
extern int printf(string fmt, ...);
```

A user-defined variadic function reads its extra arguments with `va_list` and the
builtins `va_start(ap)`, `va_arg<T>(ap)` (one argument of type `T`), and
`va_end(ap)`:

```eskiu
int sum(int n, ...) {
    va_list ap;
    va_start(ap);
    int total = 0;
    for (i in 0..n) { total = total + va_arg<int>(ap); }
    va_end(ap);
    return total;
}
```

The C default argument promotions apply to variadic arguments: a `float` is passed
as `double` (read it with `va_arg<double>`), and integer types narrower than `int`
arrive as `int`. There is no automatic count of the arguments: pass it explicitly
(as `n` above) or use a sentinel.

`va_start` is only allowed in a function with a `...` parameter. Each of the three
builtins takes exactly one `va_list` operand (a `va_list` may also be passed to another
function, which then reads it with `va_arg`). `T` in `va_arg<T>` must be a scalar: an
integer, a floating-point type or a pointer; a struct, union, sum type or array is a
compile error. So is a type the promotions widen (`va_arg<float>`, `va_arg<char>`,
`va_arg<int8>`, ...): read the promoted `double` or `int` and convert it.

### 6.4 Extern Declarations

`extern` declares a C function or global variable available to Eskiu code. See §13 for details.

### 6.4.1 Intrinsic Declarations

`intrinsic` declares a function whose implementation the compiler supplies directly, rather than one defined in Eskiu or linked from C. The form is a prototype with no body:

```eskiu
intrinsic int  atomic_load(*int cell);
intrinsic void atomic_store(*int cell, int v);
```

A call to an intrinsic lowers to a specific instruction sequence chosen by codegen (the `<atomic>` intrinsics lower to LLVM atomic loads, stores, `atomicrmw`, `cmpxchg` and `fence` with fixed orderings) instead of an ordinary `call`. The compiler knows one family: `atomic_<op>` on an `int` cell, `atomic_<op>64` on an `int64` cell and `atomic_<op>_ptr` on a pointer cell, where `<op>` is `load` (acquire), `store` (release), `swap`, `cas` (acq_rel; a failed `cas` acquire), or, for the integer cells, `fetch_add`, `fetch_sub`, `fetch_and`, `fetch_or`, `fetch_xor` (acq_rel, returning the previous value), plus `atomic_fence()` (seq_cst). Each of them except the fence also exists with a `_relaxed` suffix after the cell kind (`atomic_fetch_add_relaxed`, `atomic_load64_relaxed`, `atomic_cas_ptr_relaxed`): the same atomic operation with no ordering (LLVM `monotonic`, C's `memory_order_relaxed`), for counters and statistics that are read after a join. Any other name is an error. Intrinsics are how the standard library exposes operations that have no portable C spelling; application code rarely declares its own.

### 6.5 Lambdas and Closures

An anonymous function (lambda) is written with a C-style function body and no name. The syntax is identical to a named function declaration without the name:

```eskiu
int(int x) { return x * 2; }
```

The type of a lambda is the corresponding function pointer type `fn(T,...)->R`. Lambdas are assigned to variables, passed as arguments, or used anywhere a function pointer value is expected.

```eskiu
// Assign to a variable
let double_it: fn(int)->int = int(int x) { return x * 2; };
int result = double_it(5);   // result == 10

// Pass as an argument
int apply(fn(int)->int f, int x) {
    return f(x);
}
int out = apply(double_it, 4);   // out == 8

// Inline (pass directly)
int out2 = apply(int(int x) { return x + 1; }, 9);  // out2 == 10
```

**Closure capture.** A lambda may reference variables from its enclosing scope. Such variables are captured by value at the point the lambda expression is evaluated.

```eskiu
int base = 10;
let add: fn(int)->int = int(int x) { return x + base; };
add(5);   // 15: 'base' was captured by value
```

The closure holds its own copy of each captured variable, taken when the lambda expression is evaluated: a later change to `base` in the enclosing function is not seen by `add`. For the same reason a lambda may not assign to a captured variable (`base = 1;`, `base += 1;` or `base++;` inside the body is a compile error, "cannot assign to captured variable"; so is a write to a field or element of a captured struct or array value, `p.a = 5;` or `arr[0] = 9;`), since the write would change only the copy and be lost. Taking the address of a captured variable's storage (`&base`, `&p.a`, `&arr[0]`) or slicing a captured array (`arr[0..2]`) is an error for the same reason ("cannot take the address of captured variable"). To share state with the enclosing code, write through a pointer to it (`*p = v`, `ptr.a = v`), or use a global or a `static` local: those are not captured, the lambda reads and writes the one variable. The lambda's own parameters and locals are ordinary variables it may assign. A captured value is read-only inside the lambda, as with a by-value capture in C++: calling a method whose receiver is a plain `*P self` on a captured struct value or on a field of one (`p.set(5)`, `w.inner.set(5)`, a generic dot-call such as `l.push(8)`, or an inline method) is an error ("cannot call method 'set' on captured variable 'p'"), since the method would modify only the copy. A method declared with `const P* self` may be called. Capture a pointer (`*P pp = &p;` outside the lambda, `pp.set(5)` inside) to modify the original.

Under the hood, `fn(T)->R` is a two-word fat pointer `{fn_ptr, env_ptr}`. When a lambda captures one or more variables, the compiler packages them into an environment struct and stores its address in `env_ptr`. Lambdas that capture nothing have `env_ptr = null` and compile identically to plain function pointers. The representation is fully transparent to user code. The type annotation remains `fn(T)->R` in both cases.

**Escape analysis and closure lifetime.** Where the environment lives depends on whether the closure *escapes* its creating function:

- A **non-escaping** closure (one that is only called, or passed to a parameter that is not marked `escaping`) has its environment allocated on the **stack**. This costs nothing and needs no cleanup (the common `map`/`filter`/callback-invoked-in-place case).
- An **escaping** closure (one that is returned, stored into a struct field / global / through a pointer, or passed to an `escaping` parameter) has its environment allocated on the **heap**, so it remains valid after the creating function returns. Release it with `free_closure(f)` (a no-op for non-capturing closures, whose env is null; `f` must be a closure value).
- In an `async` function a closure bound to a local lives in the coroutine frame across suspensions, so its environment is on the heap. When the local is only bound to lambdas and otherwise only called, the frame owns it: binding the local again frees the previous environment, and the last one is freed when the async function completes or is cancelled. A local used any other way keeps the escaping rule above.

A parameter that retains the closure beyond the call (stores it, returns it, hands it to another `escaping` parameter) must be declared `escaping`:

```eskiu
// stores cb beyond the call -> the parameter is `escaping`
void on_ready(int fd, escaping fn(int)->void cb) { handlers[fd] = cb; }

// only calls f -> no annotation; closures passed here stay on the stack
int apply(fn(int)->int f, int x) { return f(x); }
```

Passing a non-`escaping` closure parameter straight on to another function's non-`escaping` parameter is also fine, since that callee can only call it too. A lambda that captures a non-`escaping` parameter escapes it with the lambda unless the lambda itself cannot outlive the call: it is passed straight to a non-`escaping` parameter, or bound to a local that is only called. Returning such a lambda, storing it, passing it to an `escaping` parameter or to `thread_create` needs the parameter marked `escaping`.

This is checked: using a non-`escaping` closure parameter beyond a direct call is a compile error pointing you at `escaping`, so a closure can never silently outlive its stack environment. `escaping` and `free_closure` are reserved words (§2.3).

**Named functions as values.** A top-level function used as a value (rather than called) decays to a `fn(T,...)->R`, so it can be assigned or passed directly, no lambda wrapper needed:

```eskiu
void worker() { /* ... */ }
int  inc(int x) { return x + 1; }

*void t = thread_create(worker);   // pass the function itself
int r   = apply(inc, 41);          // r == 42
```

The compiler synthesizes a tiny adapter so the function fits the `{fn_ptr, env_ptr}` calling convention (the function does not take an environment); this is transparent to your code.

### 6.6 Template Functions

Template functions are parameterized by one or more type variables declared in angle brackets after the function name:

```eskiu
T identity<T>(T x) {
    return x;
}

T max<T>(T a, T b) {
    if (a > b) return a;
    return b;
}
```

Called with explicit type arguments:

```eskiu
int result = max<int>(3, 5);
float fmax = max<float>(1.5, 2.5);
int same = identity<int>(42);
```

The type arguments may also be **inferred** from the argument types. Inference works both when a type parameter appears directly as a parameter type and when it appears inside a composite parameter type, so type arguments can be omitted in either case:

```eskiu
int r = max(3, 5);            // T inferred as int (direct parameter)
int i = identity(42);         // T inferred as int

let nums: List<int>;
List_init(&nums, 4);          // T inferred as int from the List<int>* argument
List_push(&nums, 7);
int first = List_get(&nums, 0);
```

Inference unifies each parameter type against the concrete argument type
structurally (peeling pointers, matching template instances and slice element types),
so a `List<T>*` parameter binds `T` from a `List<int>*` argument and a `T[]` parameter
binds `T` from an `int[]` slice (`sum(a[0..4])`). If a type parameter cannot be
inferred from any argument, pass the type arguments explicitly. Each unique set
of type arguments generates a separate monomorphic instantiation.

A binding from a composite parameter type (`*T`, `List<T>*`) takes precedence. When a
type parameter is bound only by parameters spelled as the bare type parameter (`T a, T
b`), every such argument must deduce the same type. Integer deductions that differ meet
at their common type by the usual arithmetic conversions (section 3.1), so
`max(1, big)` with `int64 big` is `max<int64>` and does not truncate; any other
disagreement (`max(1.5, (float)2.5)`, `pick(1, "s")`) is a type error. Once the type
arguments are known, every argument is checked against the instantiated parameter type,
as for any call (`swap(&i64, &i32)` against `*T` is an error).

### 6.7 Thread Primitives

`thread_create` and `thread_join` are language keywords that spawn and await OS threads.

```eskiu
thread_create(fn()->void worker) -> *void
thread_join(*void handle) -> void
```

`thread_create` accepts any `fn()->void` value, including a closure, and returns an opaque `*void` thread handle. `thread_join` blocks the calling thread until the spawned thread completes. Any other worker type, or a `thread_join` operand that is not a `*void` handle, is a compile error.

```eskiu
import <io>;

int main() {
    *void t = thread_create(void() { printf("hello from thread\n"); });
    thread_join(t);
    return 0;
}
```

**With a capturing closure:**

```eskiu
int id = 1;
let worker: fn()->void = void() { printf("thread %d\n", id); };
*void t = thread_create(worker);
thread_join(t);
```

**Ownership.** A lambda written in the call (`thread_create(void() { ... })`) belongs to the new thread: its environment is freed when the thread body returns. A closure value passed in (`thread_create(worker)`) stays its owner's, since one closure may start several threads; release it with `free_closure(worker)` after the last `thread_join`.

**Implementation detail.** For a closure value, the fat pointer `{fn_ptr, env_ptr}` maps directly to the `(start_routine, arg)` pair expected by `pthread_create`. A lambda written in the call starts through a small trampoline that runs it and then frees its environment. When a program calls `thread_create`, the driver links `-lpthread` on Linux and Windows (mingw) by itself; macOS has pthread in libSystem.

### 6.8 Async Functions and `await`

An `async` function lowers to a resumable state machine and executes over the
`<eventloop>`/`<executor>` runtime. Single and multiple awaits, `async void`, and every
control-flow construct containing an await (`if`/`while`/`do`-`while`/C-style
`for`/`switch`/`match`/`for-in`, with `break`/`continue`, and `try`/`catch`) are
supported; a pending future is cancelled with `Future_drop`.

A task suspends only at an `await`: everything between two `await`s runs without
interruption, and a function can only `await` if it is declared `async`. The locals of an
async function live in its frame across a suspension, so memory allocated before an
`await` is still valid after it and is freed by the function's `defer`, which runs when
the function finishes, or when its future is dropped while it is waiting. The event loop
runs every task on one thread, so do not hold a `<threading>` mutex across an `await`:
another task that locks it on the same thread would block forever.

An `await` may appear anywhere in an expression: a `let` initializer, a `return` value,
an assignment or compound assignment (`x += await f();`), a call argument, an operand
(`a + await f()`), a condition (`while (await more())`), a `switch` or `match` subject, a
`for-in` iterable and a range bound (`for (i in 0..await n())`). Evaluation keeps the
order of the synchronous expression: an operand with a side effect written before the
await is evaluated before it, an assignment target (`a[i()] += await f()`) is evaluated
once, before the await, and an await in the right operand of `&&`/`||` or in an arm of
`?:` runs only when that operand is evaluated. A switch subject, a `for-in` iterable and
a range bound are evaluated once, before the statement; a loop condition or step is
evaluated on every pass.

Inside `try` an await may sit in the body, in a `catch` handler and in the `finally`. An
exception thrown before or after a suspension is caught by the handler of the try it is
thrown in, and the `finally` runs exactly once on every exit: normal completion, a caught
or uncaught exception, an early `return`, `break` or `continue`, and cancellation. A
`defer` in a block split by an await runs at every exit of that block, as in a plain
function, and its body may await too. An `await` in a `finally` or a `defer` suspends like
any other: a `return` value is kept while the cleanup awaits, several defers still run
last-registered first, and an exception unwinding through the try is thrown again once
the `finally` finishes.

A future dropped while suspended runs the `finally` blocks and `defer`s pending at its
await once, innermost first, after the future it was awaiting has been dropped. When that
cleanup awaits, it runs detached, like a task given to `spawn`: `Future_drop` returns while
the cleanup is suspended, the event loop resumes it, and its frame is freed when it ends.
Nobody reads the result of a dropped future, and dropping it again while its cleanup runs
does nothing. An exception that escapes such a cleanup propagates to whatever resumed it,
as one escaping a spawned task does, so keep the event loop running until the cleanup is
done.

Rejected with a located error: labeled `break`/`continue`, an `await` in a `sizeof`
operand, an `asm` input or a `thread_join`, and a `finally` that leaves by `break` or
`continue` (a `return` or `?` there is a type error). Bind the value first with
`let v = await ...;`. A generic async function accepts an await in the same positions as
a plain one.

An `async` function is declared with the `async` modifier before the return type. Its
*declared* return type is the value it ultimately produces, but a **call** to it
yields a `*Future<T>`, a handle to the eventual result, rather than `T` directly:

```eskiu
import <future>;
import <net_async>;

async int read_len(EventLoop* lp, int fd, *uint8 buf) {
    int n = await net_read_async(lp, fd, buf, 4096);   // suspend until readable
    return n;
}
// read_len(lp, fd, buf) has type *Future<int>
```

`await E` suspends the enclosing async function until the future `E` completes, then
evaluates to its result. `E` must have type `*Future<T>`, and `await E` has type `T`.
`await` is legal **only inside an `async` function**; at the top level you drive a
future explicitly (there is no top-level `await`). The future being awaited may come
from a leaf primitive (`<net_async>`) or from calling another async function.

`Future<T>`, the executor, and the leaf futures live in the standard library
(`<future>`, `<executor>`, `<net_async>`); the generic combinators `spawn<T>` (detach
a fire-and-forget task), `select2<A,B>` (first-of-two) and `join2<A,B>` (all-of-two)
take typed futures with no cast at the call site, `<timer>` `timer_after(lp, ms)` is a
leaf future for deadlines (so `select2(read, timer_after(...))` is a real timeout; dropping
a combinator before it resolves drops its inputs with it), and
`<http_async>` is a non-blocking HTTP server built on the accept loop. See
[async-design.md](../dev/async-design.md) for the runtime contract and the lowering design.

### 6.9 Exception Handling

Eskiu supports structured exception handling via `try`, `catch`, `finally`, and `throw`.

#### Syntax

```eskiu
try {
    // body: any function calls here are emitted as LLVM invoke
} catch (TYPE name) {
    // handler: receives the thrown value as 'name'
} finally {
    // cleanup: always executes
}
```

Multiple `catch` clauses may be chained. The `finally` clause is optional. Either `catch` or `finally` (or both) must follow `try`.

#### throw

`throw expr` throws the value of `expr` as an exception. Any Eskiu value type may be thrown: `string`, an integer of any width, `float`, `double`, a pointer, or a struct by value. The value is copied into the exception object with its own type, so a `catch` of the same type receives it unchanged. Inside a generic function the thrown type is the instance's type (`throw x` with `x: T` in `f<double>` is caught by `catch (double d)`).

```eskiu
int divide(int a, int b) {
    if (b == 0) {
        throw "division by zero";
    }
    return a / b;
}
```

#### Catching exceptions

Each `catch` clause names a type and a variable. If the thrown value's static type is the declared type (after aliases are resolved), control transfers to that clause and the variable holds the thrown value. There is no conversion: a thrown `int` is not caught by `catch (int64 e)`.

```eskiu
try {
    int r = divide(10, 0);
} catch (string e) {
    printf("caught: %s\n", e);
}
```

#### finally

The `finally` block executes unconditionally after the `try` body and any `catch` clause, regardless of whether an exception was raised. It also runs when the body or a `catch` handler leaves early with `return`, `break`, `continue` or `?`, and when a `catch` handler throws (directly or from a call); the new exception then propagates after the `finally` body.

A `finally` block may not leave the function: a `return` or a `?` inside it is a compile error, since it would discard the pending exit (a return value, or an exception being unwound). A `break` or `continue` inside it is allowed, and a lambda written there is its own function.

```eskiu
try {
    throw "error";
} catch (string e) {
    printf("caught: %s\n", e);
} finally {
    printf("cleanup\n");
}
```

#### Unhandled exceptions

If no `catch` clause matches the thrown value, the exception propagates up the call stack. If it reaches the top with no handler (the end of `main`, of a thread's function, or a C frame that cannot unwind), the program prints the exception's type, and its value for every primitive type (integers of every width, `float`, `double`, `bool`, `char`, `string`), to stderr and aborts (exit status 134 on Unix):

```
error: uncaught exception: string "config not found"
error: uncaught exception: int 42
error: uncaught exception: ParseError
```

This is the format the REPL uses. A program that never throws does not install the handler; neither does a `--freestanding` build, which has no C++ runtime.

#### Linking

Exceptions use the platform C++ runtime. When a program contains `throw` or `try`, the driver links that runtime by itself: `-lc++` on macOS and `-lstdc++` on Linux and Windows (mingw). A bare-metal target gets nothing, and `--no-default-libs` turns this off (see the Linking paragraph of §16).

```bash
eskiuc file.esk -o file            # no -l flag needed
```

---

### 6.10 Which error mechanism to use

Eskiu code reports an expected failure (a missing file, bad input, a refused connection)
with `Result<T, E>` and passes it up with `?` (§10.5). `throw` is for errors that are not
part of normal operation: a broken invariant, a state the program cannot continue from,
or a failure that has to unwind through many frames at once. A function's signature then
says whether it can fail, and `?` keeps the propagation visible at each call.

The standard library modules that wrap C report failures the way C does, with a status
code or `null` (`fetch` returns 0 or a negative `FETCH_ERR_*`, `json_parse` returns
`null`), and the most used of them also have a `_result` form that returns a `Result<T,
string>` instead: `fetch_result`, `fetch_with_result`, `fs_read_text_result`,
`fs_write_text_result`, `fs_append_text_result`, `json_parse_result`,
`process_run_result` and `sh_result`. The error string describes the failure; the
module's own error function (`fetch_last_error`, `fs_last_error`, `process_last_error`)
gives the detail. With them, `?` works across the standard library:

```eskiu
import <fs>;
import <json>;

Result<int, string> port_of(string path) {
    String text = fs_read_text_result(path)?;
    *JsonValue config = json_parse_result(text.data)?;
    int port = (int)JsonValue_as_int(json_get(config, "port"));
    JsonValue_free(config);
    String_free(&text);
    return Ok<int, string>(port);
}
```

A status code from any other call is turned into a `Result` the same way, at the call:
`if (rc != 0) { return Err<int, string>("..."); }`.

## 7. Control Flow

### 7.1 if / else if / else

```eskiu
if (x > 0) {
    printf("positive\n");
} else if (x < 0) {
    printf("negative\n");
} else {
    printf("zero\n");
}
```

The condition must evaluate to a `bool`, a number (non-zero is true; a float compares with `0.0`, so NaN is true) or a pointer, `string` and `?*T` included (non-null is true). An interface value is true when it is not `null`. A struct, array, slice or closure is not a condition. Braces around a branch body are optional: as in C, an unbraced body is a single statement (`if (x > 0) n++; else n--;`), and it is its own scope (see §7.8 for what that means for `defer`).

### 7.2 for

C-style three-part form:

```eskiu
for (int i = 0; i < 10; i += 1) {
    printf("%d\n", i);
}
```

The init clause may declare a variable scoped to the loop. Each part is optional:

```eskiu
for ( ; running; ) {
    // condition only
}
```

### 7.2.1 for ... in (for-each)

The `for (x in iterable)` form binds `x` to each element of `iterable` in turn.
The loop variable is a fresh copy each iteration; assigning to it does not modify
the underlying collection. `break` and `continue` work as in any loop.

Four kinds of iterable are supported:

- **Half-open integer ranges** `A..B`: iterate `A, A+1, …, B-1` (the upper bound
  is exclusive). `A` and `B` are any integer expressions:

  ```eskiu
  for (i in 0..10) {        // 0,1,…,9
      printf("%d\n", i);
  }
  ```

  A range desugars to `for (T i = A; i < B; i = i + 1)`, so an empty range
  (`A >= B`) runs zero times. `B` is evaluated once, before the first iteration:
  `for (i in 0..n())` calls `n()` once, and changing a variable used in `B` inside
  the body does not change the number of iterations.

  Both bounds are read in the enclosing scope, before the loop variable exists. A
  bound that names the loop variable means the outer variable of that name, as in C's
  `for (int i2 = a; i2 < b; ...)`: with an outer `int i = 3`, `for (i in 0..i)` runs
  `0, 1, 2`. When `B` names the loop variable it is evaluated before `A`; otherwise
  `A` is evaluated first.

  The loop variable's type `T` is the common type of the two bounds under C's usual
  arithmetic conversions: each bound is promoted to at least `int`, the wider one
  wins, and at equal width an unsigned bound makes `T` unsigned. So `0..n` with
  `n: int64` counts in `int64`, and two `uint8` bounds count in `int`. A bare
  integer literal bound takes the first of `int`, `int64`, `uint64` that holds its
  value. A bound that is not an integer (a `float`, a pointer) is a compile error.

- **Fixed-size arrays** (`T[N]`), including array fields. Over a multidimensional
  array `T[N][M]` the loop variable is each row, a `T[M]`:

  ```eskiu
  int[4] xs;
  xs[0] = 10; xs[1] = 20; xs[2] = 30; xs[3] = 40;
  for (v in xs) {
      printf("%d\n", v);
  }
  ```

- **Slices** (`T[]`): a fat pointer that carries its length, iterated element by
  element through its data pointer:

  ```eskiu
  int[4] xs;
  xs[0] = 10; xs[1] = 20; xs[2] = 30; xs[3] = 40;
  int[] s = xs[1..3];       // a view of xs[1], xs[2]
  for (v in s) {
      printf("%d\n", v);    // 20, 30
  }
  ```

- **List-like structs**: any struct with an `int size` field and a `data`
  pointer field, which includes `List<T>` from the standard library:

  ```eskiu
  import <list>;
  let nums: List<int>;
  List_init(&nums, 4);
  List_push(&nums, 1);
  List_push(&nums, 2);
  for (n in nums) {
      printf("%d\n", n);
  }
  ```

The form desugars to an index-counted loop: for an array the bound is its
compile-time length; for a slice the bound is its `.len`; for a List-like value
the bound is its `size` field, and each element is read through `data[i]`.

The iterable is evaluated once, before the first iteration. A variable, or a field or
constant-index element of one, is read in place, so the loop sees writes the body makes
to it. Anything else (a call, an index by a variable) is held in a temporary: a
List-like value that has an address by a pointer to it, an array, slice or pointer by
value (an array is then copied).

### 7.3 while

```eskiu
while (condition) {
    // body
}
```

The body executes repeatedly while `condition` is true.

### 7.3.1 do / while

```eskiu
do {
    // body
} while (condition);
```

Like `while`, but the condition is tested *after* the body, so the body always runs at
least once. `break` and `continue` work as in the other loops (`continue` re-tests the
condition).

### 7.3.2 Labeled break / continue

A plain `break` or `continue` acts on the innermost enclosing loop. To act on an outer
loop from inside a nested one, give the outer loop a label and name it:

```eskiu
outer: for (int i = 0; i < rows; i = i + 1) {
    for (int j = 0; j < cols; j = j + 1) {
        if (grid[i][j] == target) {
            found = 1;
            break outer;        // leave both loops
        }
    }
}
```

A label is an identifier followed by `:` directly before a `while`, `do`/`while`, `for`,
or `for ... in` loop. `break label` leaves that loop; `continue label` skips to its next
iteration (for a `for`, its step runs first, as with an unlabeled `continue`). The label
must name an enclosing loop, otherwise the program is rejected at compile time.

Labeled `break` and `continue` run the same `defer`/`errdefer` cleanups as the unlabeled
forms: every deferred statement between the jump and the target loop runs, innermost
first, before control leaves. A labeled jump may not escape a `defer` body, and labeled
`break`/`continue` is not supported inside an `async` function.

### 7.4 switch / case / default / break

```eskiu
switch (x) {
    case 1:
        printf("one\n");
        break;
    case 2:
        printf("two\n");
        break;
    default:
        printf("other\n");
        break;
}
```

`switch` dispatches on an integer value. `break` exits the enclosing switch. If `break` is omitted, control falls through to the next case. The type checker validates that each `case` value is compatible with the type of the `switch` subject expression.

A declaration may follow a `case` or `default` label directly, without braces. As in C, the whole switch body is one scope: the variable is visible from its declaration to the end of the switch, including the cases after it, and two cases may not declare the same name. A jump to a later case skips the initialization, so the variable is uninitialized there until assigned (this is not diagnosed).

### 7.5 return

Returns a value from the current function. A `void` function uses `return;` with no operand, or `return f();` with a `void` call.

```eskiu
int sign(int x) {
    if (x > 0) return 1;
    if (x < 0) return -1;
    return 0;
}
```

### 7.6 break

Exits the innermost enclosing loop (`for`, `for ... in`, `while`, `do`/`while`) or `switch` immediately.

```eskiu
while (true) {
    if (done) break;
}
```

### 7.7 continue

Skips the remainder of the current loop iteration and proceeds to the next:

```eskiu
for (int i = 0; i < 10; i += 1) {
    if (i % 2 == 0) continue;
    printf("%d\n", i);   // prints odd numbers only
}
```

### 7.8 defer

`defer stmt;` schedules `stmt` (a single statement or a `{ ... }` block) to run when the
enclosing block is left. It is the ergonomic way to pair a resource acquisition with its
release: write the cleanup right next to the thing it cleans up, and it runs on **every**
path out of the block, so you never leak on an early return or a propagated error.

```eskiu
Result<int, string> read_into(*uint8 buf, string path);

Result<int, string> process(string path) {
    *uint8 buf = alloc<uint8>(4096);
    defer free(buf);                        // runs however this function exits

    if (path[0] == 0) { return Err<int, string>("empty path"); }   // buf freed
    int n = read_into(buf, path)?;          // buf freed if the `?` propagates an error
    return Ok<int, string>(n);              // buf freed
}
```

`?` needs the enclosing function to return a `Result` (§10.5), so the example returns
`Result<int, string>`.

Rules:

- Deferred statements run in **LIFO** order: the last one registered runs first.
- `defer` runs on fall-through, `return`, `break`, `continue`, and `?`-propagation. It is
  block-scoped, so a `defer` in a loop body runs at the end of **each iteration**. An
  unbraced body (`if (c) defer f();`, a single-statement loop body, a `switch` case, a
  `match` arm) is its own scope too: the defer runs when that body ends, and only if it
  was reached.
- The deferred statement is evaluated when the block is left (it reads variables' values
  at exit time), after any `return` value has been computed.
- A defer body may not `return`, or `break`/`continue` out of itself (that would jump out
  of the cleanup); doing so is a compile error.

`defer` also runs when an exception unwinds out of the block: a `defer` in a `try` body
runs before that `try`'s `catch` handles the exception, and a `defer` in a function the
exception propagates through runs when an outer `try` catches it. (An exception no `try`
catches ends the program without running cleanups, as in C++.) An `errdefer` does not run
on an exception, only on `?`-propagation.

`defer` complements `try`/`finally` (§6.9): `finally` is for catch-and-cleanup around a
block, while `defer` colocates a one-off release with its acquisition.

**`errdefer`** is a variant that runs its statement only on the **error exit** path,
namely when the function leaves through `?`-propagation (an `Err` is returned early). It
does not run on a normal `return`, on fall-through, or on `break`/`continue`. This is the
tool for undoing partial work when a fallible step fails partway through, while keeping it
on success:

```eskiu
Result<int, string> connect(string host);
Result<int, string> handshake(int fd);
extern int close(int fd);

Result<int, string> open_ready(string host) {
    int fd = connect(host)?;
    errdefer close(fd);             // closed only if a later `?` fails; kept on success
    handshake(fd)?;                 // if this errors, close(fd) runs and the Err propagates
    return Ok<int, string>(fd);     // success: fd stays open, errdefer does not run
}
```

On the error path both kinds run in LIFO order (an `errdefer` registered after a `defer`
runs first). All the defer-body restrictions above apply to `errdefer` as well.

---

## 8. Structs

### 8.1 Declaration with Fields

```eskiu
struct Point {
    float x;
    float y;
}

struct Rect {
    float x;
    float y;
    float width;
    float height;
}
```

Field types may be any primitive type, pointer type, another struct type, or a fixed-size array type.

An integer field may declare a **bit width** with `: N`, making it a bitfield. Bitfields are laid out like C on the target: on SysV/AAPCS targets a bitfield takes the next free bits unless it would cross a boundary of an aligned storage unit of its declared type (so `uint8 a : 4; uint32 w : 12;` is 4 bytes), and in a `packed` struct bitfields pack back to back; on Windows targets consecutive bitfields share a storage word only while the declared type size stays the same. Reads mask and shift out the field (signed fields sign-extend); a bitfield whose values all fit an `int` (fewer than 32 bits, or at most 32 for a signed one) is read as an `int`, as C promotes it, so `u - 1` of a `uint32 u : 3` holding 0 is -1 (a postfix `f++` keeps the declared type, as in clang). Writes (including compound assignment and `++`/`--`, which wrap within the field's width) are read-modify-write. You cannot take the address of a bitfield. A `bool` bitfield uses a one-byte storage unit, as in C. An enum bitfield reads back unsigned when the enum has no negative member (SysV/AAPCS, as clang and GCC do; the Windows layout keeps it signed); a sum type is not a bitfield type. A named bitfield may not have zero width (`int x : 0` is an error).

A bitfield may have no name, as in C. `T : N;` reserves N bits laid out exactly like a named bitfield of type T, and `T : 0;` moves the next bitfield to the next allocation unit of T (`uint8 a : 3; uint32 : 0; uint8 b : 2;` puts `b` 4 bytes in on SysV). An unnamed bitfield is not a member: it cannot be read or written, a positional struct literal skips it, and its bits are zero in a literal. Its type must be an integer type at least N bits wide. Whether it changes the struct's alignment follows the target's C rules: on AAPCS (32-bit ARM, and AArch64 outside Apple platforms) it raises the alignment to its type's, elsewhere on SysV it does not; on Windows a nonzero one takes a storage word like a named one, and a `: 0` counts only right after another bitfield. A union may also hold unnamed bitfields, which only add to its size (and, on AAPCS, its alignment).

```eskiu
struct Flags {
    uint32 ready  : 1;
    uint32 mode   : 3;
    uint32 weight : 4;
}

let f: Flags = Flags { ready: 1, mode: 5, weight: 9 };
f.mode = 2;   // leaves ready and weight untouched
```

### 8.2 Methods with Implicit self

Methods are declared inside the struct body. Inside a method body, `self` refers to a pointer to the receiver struct. Because `self` is that implicit parameter, an inline method may not declare a parameter named `self`.

```eskiu
struct Counter {
    int count;

    void increment() {
        self.count += 1;
    }

    int get() {
        return self.count;
    }
}
```

Methods are lowered to regular functions with a leading pointer parameter, e.g., `Counter_increment(*Counter self)`. A method may also be written at top level in that lowered form, `int Counter_get(*Counter self) { ... }`, and it is called the same way (`c.get()`). To call a method on a `const` value, write the receiver as `const T* self` (see §4.6).

Dot syntax also reaches a **generic** free function on an instance of a generic struct. When `x` has type `S<A..>` (or `*S<A..>`) and there is a generic function `S_m<T..>` whose first parameter is the struct (`S<T..>*`, or `S<T..>` by value), then `x.m(args)` is the call `S_m<A..>(&x, args)`, or `S_m<A..>(x, args)` when `x` is already a pointer. The type arguments come from the receiver: a type parameter named in the first parameter's type takes the receiver's argument at that position, and any other one is inferred from the call's arguments. The arguments are checked against the instantiated parameter types, as for any call. This is how the standard library's generic containers read:

```eskiu
import <list>;

List<int> l; l.init(4);      // List_init<int>(&l, 4)
l.push(8);                   // List_push<int>(&l, 8)
int n = l.len();             // List_len<int>(&l)
int first = l.get(0);        // List_get<int>(&l, 0)
l.free();
```

It works the same way inside a generic body, where the receiver's type arguments are the enclosing instance's (`void Box_twice<T>(Box<T>* self, T x) { self.set(x); }`). An inline method declared in the struct body takes precedence over a free function of the same name.

### 8.2a Operator Overloading

A type can give meaning to an operator by declaring `operator <op>`, so `a + b` reads as algebra instead of a nested call. This is aimed at value types like vectors and matrices, where the notation carries the meaning.

```eskiu
struct V3 { float x; float y; float z; }

V3 operator +(V3 a, V3 b)     { let r: V3; r.x = a.x + b.x; r.y = a.y + b.y; r.z = a.z + b.z; return r; }
V3 operator *(V3 a, double s) { let r: V3; r.x = a.x * (float)s; r.y = a.y * (float)s; r.z = a.z * (float)s; return r; }
V3 operator -(V3 a)           { let r: V3; r.x = 0.0 - a.x; r.y = 0.0 - a.y; r.z = 0.0 - a.z; return r; }

V3 p = (q + t) * 2.0;   // operator + and then operator *(V3, double)
V3 n = -q;              // the unary operator -
```

Overloadable: the binary operators `+ - * / % == != < > <= >= & | ^ << >>`, the unary operators `- ! ~`, and subscript `[]`. A binary operator and `[]` take exactly two parameters, `!` and `~` exactly one, and `-` one (negation) or two (subtraction); any other count is a compile error. Compound assignment (`v += w`) is defined as `v = v + w`, using the overloaded `+`. The short-circuit operators `&&` / `||`, the pointer operators `*` / `&`, and `=` / `.` are structural and cannot be overloaded.

Resolution is entirely static and structural:

- A type gets an operator simply by declaring one for it. There is no trait or `impl` block to register; if an `operator` exists for the operand types, the operator resolves to it, otherwise the operands must be built-in.
- Overloads coexist by operand type: `operator *(V3, V3)` (component-wise) and `operator *(V3, double)` (scale) are distinct, selected by the right operand.
- An `operator` declaration compiles to a normal function, and `a op b` lowers to a direct call to it. There is no dynamic dispatch and no boxing; the cost is exactly that of writing the call by hand, so it is suitable for hot numeric code.

### 8.3 Struct Initialization

**Named initialization:**

```eskiu
Point p = Point { x: 1.5, y: 2.5 };
```

**Positional initialization:**

```eskiu
Point p = Point { 1.5, 2.5 };
```

Fields are assigned in declaration order in the positional form. A field the literal leaves out takes its default, or zero when it has none (as in C), in either form, so `T{}` is the default value of `T`.

**Field defaults.** A field may declare a default after its name: `struct Opts { int timeout_ms = 30000; bool follow = true; string name = "x"; }`. The default is a constant of the field's type (the rule for a global's initializer: literals, `const` names, enum members, `sizeof`, operators over those). It is what the field holds whenever nothing sets it: a field a literal omits (`Opts{timeout_ms: 5}` keeps `follow` true), a local or global declared without an initializer (`let o: Opts;`, at each declaration, in a loop and in an `async` function too), and a struct with defaults nested in another struct or in an array (`Opts[4] all;`). Memory from `alloc<T>` or `alloc_with` is zeroed, not defaulted: write `*p = Opts{};` to apply the defaults. A field of a generic struct cannot have a default, nor can a bitfield, and an instance of a generic struct starts at zero. Assigning a literal is how a variable is reset for reuse, with no `memset`: `opts = ProcessOptions{};` clears every field, and `opts = ProcessOptions{timeout_ms: 200};` clears them and sets one. Each value is checked against its field's type (an integer literal must fit the field), and naming a field twice, naming a field the struct does not have, or giving more positional values than there are fields is an error.

```eskiu
struct P { int x; int y; double z; }
P a = P { y: 5 };        // x = 0, y = 5, z = 0.0
P b = P { 1 };           // x = 1, y = 0, z = 0.0
```

### 8.4 Field Access and Mutation

```eskiu
let p: Point;
p.x = 1.0;
p.y = 2.0;
float sum = p.x + p.y;
```

Field assignment and load both use the `.` operator. Method calls use the same syntax:

```eskiu
counter.increment();
int val = counter.get();
```

### 8.5 Fixed-size Array Fields

```eskiu
struct QRFrame {
    uint8[858] left;
    uint8[858] right;
    int size;
}
```

`uint8[858]` lowers to `[858 x i8]` in LLVM IR.

### 8.6 Union Types

A `union` declaration is identical in syntax to `struct`, but all fields share offset 0. The size of the union equals the size of its largest field, rounded up to the alignment of its most-aligned field, and the union is aligned like that field (the C layout). Accessing a field reinterprets the underlying bytes as the field's type. No explicit cast is needed.

```eskiu
union Value {
    int    i;
    float  f;
    *uint8 p;
}

let v: Value;
v.i = 42;
printf("%f\n", v.f);  // reinterprets the 4 bytes of v.i as a float
```

A `union` variable is declared exactly like a struct variable:

```eskiu
let u: Value;
u.i = 0x3F800000;   // bit pattern for 1.0f
printf("%f\n", u.f); // prints 1.000000
```

`sizeof(Value)` returns the size of the largest field: `sizeof(*uint8)` = 8 on a 64-bit target in this example.

A union literal initializes exactly one member, named or positional (`Value{f: 1.5}`, `Value{7}` sets `i`); the remaining bytes are zero. Naming two members is an error. A union literal is a constant initializer for a global or `static` when its member value is constant.

### 8.7 Enums

An `enum` declares a set of named integer constants. Members take consecutive values starting at 0; an explicit `= N` resets the running value, and the next member continues from there. `N` is an integer constant expression, as in C: literals, the members before it (of this or another enum), top-level `const` ints declared earlier, `sizeof` (§5.8), casts, the integer operators, `!`, `&&`, `||` and `?:` (`enum Flag { A = 1, B = A << 2, C }` gives `C == 5`; `D = K > 2 ? 4 : 1`). The expression is evaluated with C's typed integer rules (§3.1: the usual arithmetic conversions, 32-bit wraparound, unsigned comparison and division, the arithmetic right shift of a signed value, `sizeof` as an unsigned size), as are array dimensions and `case` labels: `((uint)3 - (uint)5) / 2 > 100` is 1 and `(1 << 31) >> 31` is -1. A value that is not constant is a compile error, and every value must fit an `int`. The enum type itself is an `int` (`i32`), so enum values work in arithmetic, comparisons, and `switch`.

```eskiu
enum Color  { Red, Green, Blue }            // 0, 1, 2
enum Status { Ok = 0, Err = 2, Pending }    // 0, 2, 3

let c: Color = Green;            // c == 1
if (c == Red) { /* ... */ }
```

Members are unscoped. `Red` is used directly, as in C. The enum name may be used anywhere a type is expected (it behaves as `int`).

A classic enum may also be consumed with `match`, which checks the dispatch is exhaustive (every variant covered, or a `_` default), so adding a variant turns every unhandled `match` into a compile error. This is the same guarantee algebraic enums get; `switch` stays available for a non-exhaustive dispatch. Two members may share a value, as in C (`enum E { A = 1, B = 1 }`); since `match` dispatches on the value, one arm covers every member with that value, and two arms for equal values are an error.

```eskiu
int dx(Color c) {
    match c {
        Red   -> { return -1; }
        Green -> { return  0; }
        Blue  -> { return  1; }
    }
    return 0;
}
```

#### 8.7.1 Algebraic enums (tagged unions)

When one or more variants carry a **payload**, the enum becomes an algebraic data type: a tagged union, not an integer. Each variant is constructed by name (with arguments for its payload; a payload-free variant is written bare, and `Unit()` is an error), and a value is destructured with `match`:

```eskiu
enum Shape {
    Circle(float),
    Rect(float, float),
    Unit,                       // a payload-free variant
}

float area(Shape s) {
    match s {
        Circle(r)  -> return 3.14 * r * r;
        Rect(w, h) -> return w * h;          // payload fields bind to w, h
        _          -> return 0.0;            // `_` matches any remaining variant
    }
}

Shape a = Circle(2.0);          // construct; payload-free variants are bare (`Unit`)
```

Algebraic enums may be **generic** and are monomorphized per instantiation, like
template structs. Where an instance of the variant's enum is expected (a declaration,
an assignment, a `return`, an argument of a function or of a generic function called
with explicit type arguments, a struct-literal field, an array-literal element, or the
payload of another variant), the variant takes that instance's type arguments, and its
payload is checked against them: `Option<int64> a = Some(42)` builds an
`Option<int64>`, `Option<Option<int64>> b = Some(Some(5))` an `Option<int64>` inside,
and a bare `None` or an under-determining `Left(7)` is accepted there. Elsewhere the type
arguments are inferred from the payload arguments when they determine them (`Some(42)`
→ `Option<int>`); otherwise (a payload-free variant like `None`, or one that
under-determines the type like `Either`'s `Left`) write them explicitly (`None<int>()`,
`Left<int, string>(7)`). Inside a generic function only a type as written is used (a
declared type, the declared return type): `return None;` in an `Option<T>` function is
`None<T>()`; an assignment or an argument there needs the type arguments written:

```eskiu
enum Option<T>    { None, Some(T) }
enum Either<A, B> { Left(A), Right(B) }

Option<int64> x = Some(42);
Option<int> y = None;
Either<int, string> e = Left(7);

match x { Some(v) -> printf("%lld\n", v);  None -> printf("none\n"); }
```

A `match` must be **exhaustive**: every variant must have an arm, or there must be
a `_` default. Otherwise it is a compile error naming the missing variants. A
variant may not appear in two arms, and a match has at most one `_` default. The arm body is any statement (often a block
or a `return`). The value is
laid out as `{ tag, payload }`, where the payload area is sized to the largest
variant. (A `match` subject is parsed without a trailing struct literal, like the
condition of an `if`; wrap it in parens if you need one.)

### 8.8 Type Aliases

`type Name = ExistingType;` introduces a name for an existing type. The alias is fully interchangeable with its underlying type. It resolves before type checking and code generation. Aliases work for any type, including pointers, arrays, slices, fn types, interfaces, enums and templates: a value of an alias type derefs, indexes, calls, boxes, matches and dispatches methods and operators exactly as a value of the target type. An alias of an array or slice given more dimensions keeps them outer (`type IS = int[]; IS[2]` is two slices).

```eskiu
type u8      = uint8;
type Bytes   = *uint8;
type IntList = List<int>;

let buf: Bytes = alloc<u8>(16);
```

`type` is contextual: it is only a keyword in the form `type Name = ...;`, so it remains usable as an ordinary identifier elsewhere.

### 8.9 Packed Structs

By default a struct is laid out with natural alignment: the compiler inserts padding so each field sits on its required boundary. A **packed** struct removes that padding: fields are placed back-to-back. This matters when a struct must match an exact on-the-wire or on-disk byte layout, or a C struct declared with `#pragma pack` / `__attribute__((packed))`.

Mark a struct packed with the `packed` qualifier:

```eskiu
struct Natural {        // sizeof == 8 (1 byte tag + 3 padding + 4 byte value)
    uint8  tag;
    uint32 value;
}

packed struct Header {  // sizeof == 5 (no padding; value starts at offset 1)
    uint8  tag;
    uint32 value;
}
```

For C source compatibility, `#pragma pack` is also honoured. It maintains an alignment stack and applies to every struct declared while in effect; `pack(1)` packs, `pack()` / `pop` restore:

```eskiu
#pragma pack(push, 1)
struct WireHeader {     // packed (sizeof == 5)
    uint8  kind;
    uint32 length;
}
#pragma pack(pop)       // subsequent structs use natural alignment again
```

`#pragma pack(1)` and `packed struct` are equivalent (fully packed, no padding). `#pragma pack(N)` for `N > 1` caps each field's alignment at `N`: a field whose natural alignment exceeds `N` is aligned to `N` instead, and the struct's total size rounds up to its own alignment (`min(max-field-alignment, N)`). This matches the C `#pragma pack(N)` ABI. For example, under `pack(4)` a `struct { char a; int64 b; int16 c; }` lays out `a@0`, `b@4`, `c@12` with `sizeof == 16`. `N` must be 1, 2, 4, 8 or 16 (as in C); any other alignment is a compile error at the pragma. Packed layout (any `N`) composes with bitfields and is reflected by `sizeof` and by every field access. A struct declared under `pack(N)` keeps its alignment `min(max-field-alignment, N)` wherever it is used, as in C: as a field or array element of another struct (declared under any packing that does not cap it lower) and as a union member. A union declared under `pack(N)` caps each member's alignment at `N` the same way: `#pragma pack(2) union U { char[5] c; int x; }` is 6 bytes and 2-aligned.

---

## 9. Interfaces

### 9.1 Declaration

An interface declares a set of method signatures. No implementation is provided.

```eskiu
interface Drawable {
    void draw();
}

interface Greeter {
    void greet();
    string name();
}
```

### 9.2 Structural Satisfaction

There is no `implements` keyword. A struct satisfies an interface if it provides all required methods with matching signatures. The check is structural and performed at the call site.

```eskiu
struct Circle {
    float radius;

    void draw() {
        printf("Drawing circle r=%.2f\n", self.radius);
    }
}

// Circle satisfies Drawable because it has draw()
```

### 9.3 Calling Interface Methods

Interface methods are called with the `.` operator:

```eskiu
void render(Drawable d) {
    d.draw();
}
```

Dispatch is performed via the vtable pointer in the fat pointer. The arguments are checked against the interface's declaration of the method (count and types), like a direct call.

### 9.4 Passing Structs as Interfaces

Passing a struct pointer to a function expecting an interface auto-boxes it into a fat pointer `{data_ptr, vtable_ptr}`:

```eskiu
Circle c;
c.radius = 5.0;
render(&c);   // &c is auto-boxed into a Drawable fat pointer
```

The same boxing happens wherever an interface-typed slot receives a struct pointer: a local (`let d: Drawable = &c;`), an assignment, a struct field, and a `return` from a function declared to return the interface. The interface value is held by value, so it can be stored and returned freely; it keeps referring to the struct it was boxed from. Passing the struct itself (`render(c)`) is a compile error: an interface refers to a struct through a pointer, so write `&c`.

`null` converts to an interface, giving the empty value `{null, null}` (in a declaration, an assignment, a return, an argument or a field). An interface value compares with `null` (`d == null`, `d != null`) and is a condition that is true when it refers to a struct (`if (d)`, `!d`, `d && ...`); it compares with nothing else. Calling a method through an empty interface value crashes, so test it first. A global or `static` interface is initialized with `null` or the address of a global struct (`Drawable d = &gc;`), a link-time constant.

### 9.5 Implementation Detail: Fat Pointer

Under the hood, an interface value is a two-word fat pointer:

```
{ ptr data_ptr, ptr vtable_ptr }
```

The vtable is a struct of function pointers, one per interface method, generated per concrete type. This is transparent to user code.

---

## 10. Templates

### 10.1 Template Structs

Template structs are declared with one or more type parameters in angle brackets:

```eskiu
struct Pair<A, B> {
    A first;
    B second;
}

struct Box<T> {
    *T value;
    int valid;
}
```

A template struct may declare methods in its body. Each method is generated per struct instance, on the first call: for `Box<int>` the method `get` becomes `Box_int_get(*Box_int self)`. A method that only makes sense for some type arguments is fine as long as no other instance calls it.

```eskiu
struct Cell<T> {
    T v;
    T get() { return self.v; }
    T twice() { return self.v + self.v; }
}

Cell<int> c = Cell<int>{ 21 };
int n = c.twice();                // 42
```

A generic function in the `Type_method` form (`T Cell_peek<T>(Cell<T>* self)`) is also callable as `c.peek()`, with its type arguments taken from the receiver (§8.2).

### 10.2 Template Functions

```eskiu
T identity<T>(T x) {
    return x;
}

T max<T>(T a, T b) {
    if (a > b) return a;
    return b;
}
```

### 10.3 Instantiation

Template instantiation is lazy and monomorphic. The compiler generates one concrete struct or function definition per unique set of type arguments. The generated name uses underscores: `Result<int, string>` becomes `%Result_int_string` in LLVM IR.

```eskiu
let p: Pair<int, float>;
p.first = 1;
p.second = 3.14;

int big = max<int>(10, 20);
```

The body of a template is type-checked once per instance, with the concrete type arguments substituted, after the rest of the program. An error that exists only for some type arguments (an operator the concrete type lacks, a value passed where a method takes a pointer) is reported at the offending expression, with the instance named:

```
error: f.esk:3:28: invalid operands for operator: struct:Box and struct:Box (in instantiation of twice<Box>)
```

### 10.4 Using Result<T,E> from stdlib

```eskiu
import <result>;
import <io>;

int main() {
    let r: Result<int, string> = Ok<int, string>(42);
    if (r.ok) {
        printf("value: %d\n", r.value);
    } else {
        printf("error: %s\n", r.error);
    }
    return 0;
}
```

### 10.5 The `?` error-propagation operator

The postfix `?` operator removes the boilerplate of checking a `Result` after
every fallible call. Applied to a `Result<T, E>` value, `expr?`:

- if the result is `Err`, **returns the error** from the enclosing function;
- otherwise evaluates to the unwrapped success value of type `T`.

It may only appear inside a function that returns a `Result` with the same error type
`E`. The success type may differ: `?` on a `Result<String, E>` inside a function returning
`Result<int, E>` returns an `Err` of the function's own type holding the same error. The
compiler rejects `?` anywhere else, since there would be nothing to propagate into.

```eskiu
import <result>;

Result<int, string> divide(int a, int b) {
    if (b == 0) { return Err<int, string>("division by zero"); }
    return Ok<int, string>(a / b);
}

// Without `?`, each call would need its own `if (!r.ok) return r;`.
Result<int, string> compute(int a, int b, int c) {
    let x: int = divide(a, b)?;   // unwraps, or returns the Err
    let y: int = divide(x, c)?;
    return Ok<int, string>(y + 1);
}
```

A value is treated as Result-like if it has an `ok` field of an integer type or
`bool` (zero is the error) and a `value` field; the standard library's `Result<T, E>` satisfies this. Applying `?` to an
expression of any other type is a compile error.

### 10.6 Bounded type parameters (constraints)

A type parameter may carry one or more interface constraints, written after a
colon. The constraint requires that every concrete type substituted for the
parameter satisfy the named interface(s), the same structural match used for
`interface` values (§9): the type must provide a method for each signature in the
interface.

```eskiu
interface Ord {
    int cmp(*Ord other);    // `Ord` here stands for the implementing type
}

// `T` must satisfy `Ord`, checked at the call site, not deep in codegen.
T max<T: Ord>(T a, T b) {
    if (a.cmp(&b) > 0) return a;
    return b;
}

// Multiple constraints with `+`.
struct Cache<K: Hashable + Eq, V> {
    *K  keys;
    *V  vals;
    int len;
}
```

Inside an interface, a parameter type spelled with the interface's own name stands for
the implementing type (§9), so `struct Num { int v; int cmp(*Num o) {...} }` satisfies
`Ord` above. There is no `Self` keyword: `*Self` is an ordinary (unknown) type name and
no struct matches it.

The constraint is enforced when the template is instantiated. If the concrete
type does not satisfy the interface, the compiler reports the error at the
instantiation site:

```
error: main.esk:9:15: type 'int' does not satisfy constraint 'Ord' (required by a bounded type parameter): missing method 'cmp'
```

Constraints are checked for both explicit (`max<Num>(...)`) and inferred
(`max(a, b)`) instantiations, and for template structs the moment a concrete
`Name<...>` type is resolved. They do not affect name mangling; instances are
still keyed on the concrete type arguments (§10.3).

**Primitives via free functions.** A `struct` satisfies a constraint by defining
the interface's methods. A *primitive* type (`int`, `float`, …) has no methods, so
it satisfies a constraint through a **free function** named like the interface
method whose first parameter is that primitive, e.g. `int cmp(int, int)` makes
`int` satisfy `interface Ord { int cmp(Ord other); }`. Inside a generic body a
constrained call `t.cmp(x)` on such a `t` lowers to `cmp(t, x)`. So both
`max<T: Ord>(int…)` and a constraint-bounded `Map<K: Hashable, V>` over `int` keys
type-check and compile. (The function-pointer `HashMap<K, V>` from the standard
library remains for cases where you want to thread `hash`/`eq` explicitly.)

---

## 11. Memory

### 11.1 Stack Allocation

All variables declared in a function body are allocated on the stack. Stack memory is reclaimed automatically when the enclosing function returns. There is no garbage collector.

A local declared without an initializer starts at zero, like a global, a `static` local and `alloc<T>`: every field of a struct (pointers, strings and function values are null), every element of an array, a union and a slice. Each declaration gets a fresh zero, so a variable declared inside a loop does not keep the previous iteration's value, also in an `async` function. `let o: Opts;` is therefore the zero value of `Opts`. (Unlike C, where such a local is indeterminate.) A sum-type value built from a variant is zero past that variant's fields, so equal values have equal bytes.

```eskiu
struct Point { float x; float y; }

int main() {
    int x = 10;
    Point p;
    p.x = 1.0;
    return 0;
}   // x and p are reclaimed here
```

### 11.2 Heap Allocation

Heap allocation lives in the standard library, not the language core: `import <mem>` brings in `alloc<T>` and `free`.

`alloc<T>(N)` allocates space for `N` elements of type `T` and returns a `*T`. The memory is **zero-initialized**: a freshly allocated `*T` is null, an `int` is `0`, a `List` is a valid empty list, so reading a field before you assign it is defined behavior, not a garbage read (the same guarantee C++'s `new T()` gives). In hosted mode (the default) it calls `calloc(N, sizeof(T))`. Under `--freestanding` (see §11.5) it calls the user-provided `esk_alloc` instead, whose contract is likewise to return zeroed memory; the same source, selected at compile time via the `__ESKIU_FREESTANDING__` macro.

`free(ptr)` releases a heap-allocated pointer (libc `free` hosted, `esk_free` freestanding). It takes a `*void`; any pointer type coerces.

```eskiu
import <mem>;

*uint8 buf = alloc<uint8>(1024);
// ... use buf ...
free(buf);
```

`alloc<T>`/`free` are ordinary generic stdlib functions. There is no `alloc` keyword. (`alloc_with`, the explicit-allocator primitive, *is* a built-in; see §11.5.) Every allocation must be paired with exactly one `free`. Double-free and use-after-free are undefined behaviour.

### 11.3 Pointer Arithmetic

Pointer arithmetic is typed: `p + n` on a `*T` pointer advances by `n * sizeof(T)` bytes. `*void` and `*char` are byte-level (stride 1).

```eskiu
*uint8 buf = alloc<uint8>(256);
*uint8 ptr = buf + 64;    // 64 bytes from start
*uint8 back = ptr - 32;   // 32 bytes back

*int pi = alloc<int>(8);
*int  p2 = pi + 1;        // 4 bytes forward, next int element
```

`p - q` between two pointers counts the elements between them (an `int64`). Both must
point to the same type, as in C (`const`, `?` and aliases aside); `*int - *char` is a
compile error.

The subscript operator `ptr[i]` reads or writes the `i`-th element and is exactly equivalent to `*(ptr + i)` (typed by the pointee). It is the idiomatic way to index allocated buffers and array fields:

```eskiu
*int xs = alloc<int>(4);
xs[0] = 10;
xs[3] = xs[0] * 2;        // same as *(xs + 3) = *(xs + 0) * 2
```

### 11.4 Null Checks

```eskiu
let p: *int = null;
if (p != null) {
    int val = *p;
}
```

Dereferencing `null` is undefined behaviour. The compiler does not insert null checks.

### 11.5 Freestanding Mode

Passing `--freestanding` predefines the macro `__ESKIU_FREESTANDING__`, which `<mem>` uses to switch the allocation backend:

| `<mem>` function | Hosted (default) | Freestanding (`--freestanding`) |
|------------------|------------------|---------------------------------|
| `alloc<T>(n)`    | `calloc`         | `esk_alloc` (must zero)          |
| `free(p)`        | `free`           | `esk_free`                      |

In freestanding mode the user must provide `esk_alloc` and `esk_free` in their own code (typically in a kernel or bare-metal runtime); `<mem>` declares them `extern` and the linker resolves them from the user-supplied object file. To keep `alloc<T>`'s zero-initialization guarantee, `esk_alloc` must return zeroed memory (the bump allocator in the repository's `kernel/alloc.esk` does this). Code that needs heap allocation still just writes `import <mem>` and calls `alloc<T>`/`free`. The same source compiles for both modes.

```eskiu
// user-provided in kernel.esk or a C shim
*void esk_alloc(int64 nbytes) { return buddy_alloc(nbytes); }
void  esk_free(*void ptr)  { buddy_free(ptr); }
```

Freestanding mode does not remove any other language features. The standard library modules (`stdlib/result.esk`, etc.) remain available but must not import libc functions that are absent from the target.

**Custom allocators (`alloc_with`).** `alloc_with(&allocator, T, n)` is the explicit-allocator form of `alloc`: instead of going to `malloc`/`esk_alloc`, it calls `<Type>_alloc(&allocator, n * sizeof(T))` and returns a `*T`. Any struct that exposes a method `*void <Type>_alloc(<Type>* self, int64 nbytes)` is a valid allocator, so allocation strategy is a plain value, not a global. When `n * sizeof(T)` does not fit a signed 64-bit size (or the allocator's narrower size parameter), or `n` is negative, `alloc_with` yields `null` without calling the allocator. `n` must be an integer, the allocator must be a pointer to it (`&a`, or a `*Type` value), its type must have the `alloc` method (a free `<Type>_alloc` or an inline `alloc`) of that shape (the allocator, then an integer size, returning a pointer), and `T` must be a known type with a size (not `void`); otherwise it is a compile error. The returned block is zeroed, as with `alloc<T>`, whatever the allocator returns: memory a `Bump` hands out again after `Bump_reset`, or a reused `Pool` block, starts at zero.

```eskiu
import <alloc>;

*uint8 backing = alloc<uint8>(4096);   // one slab from the host (or a static buffer in freestanding)
let a: Bump;  Bump_init(&a, backing, 4096);
*int xs = alloc_with(&a, int, 16);     // 16 ints carved from the slab, no per-object malloc
```

The `<alloc>` module ships four allocators, all built on caller-provided memory (so they work under `--freestanding` with no libc `malloc`):

| Allocator | Strategy | Reclaim |
|-----------|----------|---------|
| `Bump`    | monotonic offset into the buffer | `Bump_reset` frees everything at once; individual frees are no-ops |
| `Arena`   | bump with checkpoints | `Arena_save`/`Arena_restore` free back to a marker; `Arena_reset` frees all |
| `Pool`    | fixed-size blocks, free list threaded through freed blocks | `Pool_free` returns a block for reuse |
| `FirstFit`| general-purpose, first-fit search with region splitting (after Thompson's original) | `FirstFit_free` returns a region and merges it with adjacent free regions (the free list is kept in address order); freeing null is a no-op |

### 11.6 MMIO and volatile

See §4.5 for the `volatile` qualifier. In freestanding/kernel contexts, hardware registers are accessed through `volatile` pointer variables:

```eskiu
volatile let uart_dr: *uint8 = (uint8*) 0x3F8;   // UART data register
volatile let uart_sr: *uint8 = (uint8*) 0x3FD;   // UART status register

void uart_putc(uint8 c) {
    while ((*uart_sr & 0x20) == 0) {}   // spin until TX ready
    *uart_dr = c;
}
```

Every load and store through a `volatile` pointer is emitted as a `volatile load` / `volatile store` in LLVM IR, preventing the optimiser from caching, reordering, or eliminating the access.

---

## 12. Multi-file Programs

A project can be split across files two ways: with `import` (below), or by passing several files to the compiler at once, `eskiuc a.esk b.esk -o prog`, which merges the declarations of all inputs into one program. Declaration order across files does not matter.

### 12.1 import Statement

The `import` statement has two forms:

```eskiu
import <result>;              // stdlib module, resolved by the compiler
import "stdlib/result.esk";   // local file, path relative to the importing file
import "../shared/types.esk";
```

`import <name>` resolves the module from the Eskiu installation's stdlib directory. The compiler locates the stdlib either via the `ESKIU_ROOT` environment variable (if set) or by auto-detecting it from the compiler binary's location. No path prefix is required.

`import "path"` resolves the path relative to the directory of the importing file.

**Package imports.** When the importing program belongs to a package (an `eskiu.toml` in the directory of the first input file or above), the packages listed in that package's `eskiu.lock` can be imported with the angle-bracket form:

```eskiu
import <jsonx/parse>;   // module parse of package jsonx
import <util>;          // package util, no module named
```

`import <pkg/module>` resolves to `src/module.esk` in the package's root directory when that file exists, else to `module.esk` there; a module may contain further `/` (`<pkg/sub/mod>`). `import <pkg>` resolves to `pkg.esk`, `src/pkg.esk` or `lib.esk` in the package root, the first that exists. A name without `/` that is a stdlib module stays the stdlib module, so a package named like one is reachable only with a module (`<pkg/module>`). The package itself is in its lock too, so its own modules can be imported as `<name/module>`. Inside a package, an angle-bracket import that names neither a stdlib module nor a package of the lock is an error, `unknown package 'x' (not in eskiu.toml)`, at the import. The lock is written by the package commands (`eskiuc build`, `add`, `fetch`; see the build guide); compiling a file does not fetch anything.

All declarations in the imported file (functions, structs, templates, externs) become available in the importing file.

### 12.2 Deduplication

Each file is parsed and processed at most once per compilation, regardless of how many files import it. Files are identified by their canonical path, so two different relative spellings of the same file (a diamond import) count as one. Circular imports are detected and do not cause infinite loops.

### 12.3 Example

```eskiu
// main.esk
import <io>;
import <result>;

int main() {
    let r: Result<int, string> = Ok<int, string>(0);
    printf("ok: %d\n", r.ok);
    return 0;
}
```

---

## 13. extern / C Interop

### 13.1 Extern Function Declarations

An `extern` declaration makes a C function available to Eskiu code. The declaration must match the C function's ABI exactly.

The standard library already declares the common C functions: `<io>` has `printf`, `puts` and the rest of C stdio, `<mem>` the memory and string functions (`memcpy`, `memset`, `strlen`, `strcmp`, `strncmp`, `strchr`, `strstr`), `<math>` the libm ones (`sqrt`, `pow`, ...), and `<process>` has `process_exit`. A program imports those modules and declares an `extern` itself only for other functions; an `extern` that repeats one of them with different types is a conflicting declaration.

```eskiu
import <ctypes>;
extern int printf(string fmt, ...);
extern size_t strlen(string s);
extern *void memcpy(*void dst, *void src, size_t n);
extern int open(string path, int flags);
extern void exit(int code);
```

C's `size_t`, `ssize_t` and `long` change width between targets (32 bits on wasm32,
32-bit ARM and i686; `long` also on Windows). `import <ctypes>` gives `size_t`, `ssize_t`
and `c_long`: each is an `int64` in Eskiu, and in an `extern`'s parameters and result the
compiler passes the target's C type, converting an argument to it and widening the result
back (zero-extending a `size_t`). Declare such parameters with these names; an `int64` in
their place is only right on 64-bit targets.

### 13.2 Extern Variables

An `extern` declaration with no parameter list (a type, a name, and a semicolon) names a global variable defined in another translation unit. It emits an external-linkage global with no initializer; reads and writes resolve at link time.

```eskiu
extern int   opterr;       // libc's getopt flag
extern float g_volume;     // defined in your own C code

void quiet_getopt() {
    opterr = 0;             // assign a C global
}
int getopt_reports() {
    return opterr;          // read a C global
}
```

An extern variable lives at the top level and carries no initializer (its definition, and value, live in the other object). `extern const <type> <name>;` declares a read-only C global.

The definition may also be in the same program: an `extern` next to the variable's own definition (before or after it, or in another file compiled with it) names that one variable. In the other direction, every Eskiu global has external (C) linkage under its own name, so C code can reach it with `extern int name;`. A `static` local stays private to its function.

### 13.3 Calling Extern Functions

`extern` functions are called exactly like Eskiu functions:

```eskiu
extern int printf(string fmt, ...);

int main() {
    printf("Hello, %s!\n", "world");
    return 0;
}
```

A pointer is passed to a C function as it is. The `--safe` checks and the `?*T` rules
apply to Eskiu code, not inside the C function, so a C function that does not accept
`null` must be given a pointer the program has checked: keep it as a `?*T` until an
`if (p != null)` and pass it inside that branch.

### 13.4 C ABI Compatibility

`extern` declarations emit standard C-ABI-compatible LLVM IR `call` instructions. Any function exported from a C library, including system libraries, OpenSSL, or any other C-compatible library, may be called this way.

For functions that accept or return `void*`, use `*void` on the Eskiu side:

```eskiu
extern *void malloc(size_t size);
extern *void memcpy(*void dst, *void src, size_t n);
extern *void memset(*void ptr, int value, size_t n);
```

(For heap allocation, prefer `import <mem>` and `alloc<T>`/`free` over declaring `malloc`/`free` as `extern` yourself; see §11.2.)

A struct or union may be passed to or returned from an `extern` function **by value**. The compiler lowers such a call to the target's C calling convention (register classes, homogeneous float aggregates, hidden return pointer), matching what clang emits for the same C signature, on AArch64, x86-64 System V, Windows x64, 32-bit ARM (AAPCS, soft and hard float), 32-bit x86 (cdecl) and wasm32, following `--target`. See [abi.md](../dev/abi.md) for the per-target rules.

```eskiu
struct Vec2 { double x; double y; }
extern Vec2 vec2_add(Vec2 a, Vec2 b);   // a C function taking and returning structs
```

### 13.5 Passing an Eskiu function as a C callback

Many C APIs take a function pointer (`qsort`, `signal`, OpenSSL's ALPN selector,
…). Casting a **top-level** Eskiu function to a pointer type yields its bare C
function-pointer address, the raw symbol, not the `{fn, env}` closure fat
pointer a function name otherwise decays to:

```eskiu
extern void qsort(*void base, size_t n, size_t size, *void compar);

int cmp(*void a, *void b) { return *(*int)a - *(*int)b; }

int[5] arr = {5, 3, 1, 4, 2};
qsort((*void)&arr[0], (int64)5, (int64)4, (*void)cmp);   // (*void)cmp is the raw C pointer
```

The callback's signature must match what the C side expects (C ABI). This works
only for top-level functions (a closure or fn-pointer variable still carries an
environment and is not C-callable). A callback that takes or returns a struct by
value works too: C reaches it through a generated thunk with the C calling
convention. So does one with a `va_list` parameter, such as a C library's
error-reporter callback `void (*)(void* ud, const char* fmt, va_list ap)`: the
thunk takes the `va_list` as C passes it, and the function can hand it to
`vsnprintf` or read it with `va_arg`.

An `extern` parameter of fn type is a C function pointer, so the prototype can
spell the callback's signature and the call passes a top-level function by name
(or `null`), with no cast:

```eskiu
extern void qsort(*void base, size_t n, size_t size, fn(*void, *void)->int cmp);

qsort((*void)&arr[0], (int64)5, (int64)4, cmp);
```

Passing anything else there (a lambda, a fn-typed variable) is a compile error.

---

## 14. Stdlib

Eskiu ships a set of standard library files in the `stdlib/` directory. Import a module by name with `import <name>;` (the canonical form, resolved by the compiler); `import "stdlib/name.esk";` with a relative path also works.

| File                  | Contents                                                         |
|-----------------------|------------------------------------------------------------------|
| `stdlib/result.esk`   | `Result<T,E>` template struct; `Ok<T,E>(value)` and `Err<T,E>(err)` constructor functions |
| `stdlib/list.esk`     | `List<T>` template struct; `List_init`, `List_push`, `List_get`, `List_set`, `List_remove`, `List_len`, `List_free`; scripting helpers `List_map`, `List_filter`, `List_reduce`, `List_find`, `List_any`, `List_all`, `List_contains`, `List_index_of`, `List_reverse`, `List_slice`, `List_unique`, `List_clear`, `List_pop`, `List_insert` (see [List helpers](#list-helpers)) |
| `stdlib/string.esk`   | `String` struct; `String_init`, `String_from`, `String_append`, `String_concat`, `String_push`, `String_char_at`, `String_set`, `String_clear`, `String_index_of`, `String_eq`, `String_eq_cstr`, `String_reverse`, `String_substring`, `String_from_int`, `String_to_int`, `String_cstr`, `String_len`, `String_free`, `String_starts_with`, `String_ends_with`, `String_trim`, `String_next_token` (streaming split), `String_split`/`String_split_free` (into a `List<String>`); scripting helpers `String_find`, `String_contains`, `String_replace`, `String_replace_all`, `String_split_str`, `String_lines`, `String_join`, `List_join`, `String_lower`, `String_upper`, `String_repeat`, `String_pad_left`, `String_pad_right`, `String_to_double`, `String_parse_double`, `String_from_double`, `String_reserve`, `String_appendf`, `String_format` (see [String scripting helpers](#string-scripting-helpers)). `String_to_int` accepts a leading sign and saturates at the `int` range. `String_is_space` is deprecated; use `is_space` from `<ctype>` |
| `stdlib/ctype.esk`    | Pure-Eskiu ASCII character classification (comparisons only, no libc, freestanding-safe): `is_space`, `is_digit`, `is_hex`, `is_alpha`, `is_alnum`, `is_ident_start`, `is_ident_cont`. Each takes and returns `int` (1/0) |
| `stdlib/math.esk`     | `extern` declarations for `sqrt`, `fabs`, `pow`, `floor`, `ceil`, `fmod`, `abs` |
| `stdlib/io.esk`       | `extern` declarations for `printf`, `fprintf`, `sprintf`, `scanf`, `puts`, `getchar`, `putchar`; console helpers `print`, `println`, `print_err`, `println_err`, `read_line`, `input`, `read_all_stdin`, and the C streams `io_stdin`/`io_stdout`/`io_stderr` |
| `stdlib/mem.esk`      | Heap allocation `alloc<T>(n)` / `free(p)` (libc, or `esk_alloc`/`esk_free` under `--freestanding`); plus `extern` `memcpy`, `memset`, `memmove`, `memcmp`, `strlen` |
| `stdlib/fs.esk`       | File I/O: `fs_open`, `fs_close`, `fs_flush`, `fs_read`, `fs_readline`, `fs_write`, `fs_puts`, `fs_seek`, `fs_tell`, `fs_size`, `fs_read_all`, `fs_write_all`, `fs_eof`, `fs_error`. Text files (`fs_read_text`, `fs_write_text`, `fs_append_text`, `fs_read_lines`), metadata (`FileStat`, `fs_stat`, `fs_lstat`, `fs_exists`, `fs_is_dir`, `fs_is_file`), directories (`fs_mkdir`, `fs_mkdir_p`, `fs_remove`, `fs_remove_all`, `fs_rename`, `fs_copy`, `fs_list_dir`, `fs_walk`, `fs_glob`, `fs_match`), `fs_cwd`, `fs_chdir`, `fs_temp_file`, `fs_temp_dir`, `fs_temp_base`, and errors (`fs_last_error`, `fs_errno`, `fs_errno_missing`); see [Files and directories](#files-and-directories-fs) |
| `stdlib/net.esk`      | TCP sockets: `net_tcp_listen`, `net_accept`, `net_accept_addr` (accept + peer IPv4), `net_tcp_connect` (a dotted IPv4 address, or a host name or IPv6 address resolved through `<dns>`), `net_send`, `net_recv`, `net_send_str`, `net_close`, socket timeouts (`net_set_recv_timeout`, `net_set_send_timeout`, `net_set_timeouts`, `net_would_block`; the `struct timeval` is two C `long`s, so 8 bytes on 32-bit ARM), deadlines (`net_deadline`, `NET_TIMED_OUT`) and a blocking readiness wait (`net_wait_ready`, over `poll` / `WSAPoll`) (plus the raw POSIX `extern`s and a portable `sockaddr_in`) |
| `stdlib/alloc.esk`    | Allocators over caller-provided memory for `alloc_with` (see §11.5): `Bump`, `Arena`, `Pool`, `FirstFit`, each with `_init`/`_alloc` (and `_free`/`_reset`/`_save`/`_restore` as applicable) |
| `stdlib/time.esk`     | `time_now_ms`, `time_now_s`, `time_monotonic_ms`, `time_sleep_ms`; a UTC civil calendar: `DateTime` (year/month/day/hour/min/sec plus `wday`/`yday`), `time_to_utc(t, &dt)` (epoch seconds to fields), `DateTime_to_epoch`, and `DateTime_format_iso` (ISO 8601, e.g. `2026-07-13T18:30:00Z`); local time (`time_to_local`, `time_local_offset`), `time_format`/`time_parse`/`time_parse_local` with strftime-style specifiers, `time_format_duration`, `Stopwatch` |
| `stdlib/random.esk`   | `Rng`, a seedable xoshiro256\*\* generator (not cryptographic): `Rng_seed`, `Rng_next` (raw 64-bit), `Rng_below` (unbiased `[0, n)`), `Rng_range` (`[lo, hi)`), `Rng_bool`, `Rng_double` (`[0.0, 1.0)`), `Rng_fill` (random bytes) |
| `stdlib/regex.esk`    | A Thompson-NFA regex engine run as a Pike VM (linear time, no catastrophic backtracking): `regex_compile(pattern) -> Regex` (check `.ok` / `.err`), `Regex_search(&re, text, &m)` for the leftmost match with capture groups, `Match_group`, `Match_free`, `Regex_free`, and the one-shot `regex_match(pattern, text)`. RE2 syntax and semantics (as Go's regexp) over UTF-8: `.`, classes and literals match whole code points, match offsets are byte offsets, and a byte that is not valid UTF-8 reads as U+FFFD. Literals, `.`, classes `[a-z]`/`[^...]` with POSIX names inside (`[[:alpha:]]`, `[[:^space:]]`), the ASCII shorthands `\d \w \s` (and negations; `\s` is `[\t\n\f\r ]`), Unicode classes `\pL`, `\p{Lu}`, `\p{Greek}`, `\P{..}`, `\p{^..}` (general categories, scripts, `Any`; the Unicode 15 tables are `stdlib/regex_unicode.esk`), `* + ? {m} {m,} {m,n}` (lazy with `?`; any other `{` is a literal), `|`, groups, non-capturing `(?:...)`, named `(?P<name>...)` / `(?<name>...)`, the flags `(?i)` (simple case folding) `(?m)` `(?s)` `(?U)` for the rest of a group or scoped `(?flags:...)`, `^` / `$`, `\A` / `\z`, the ASCII word boundaries `\b` / `\B`, `\xHH` / `\x{10FFFF}` and octal escapes, literal text `\Q...\E` (any other escaped letter or digit is an error) |
| `stdlib/sort.esk`     | Generic in-place heapsort `sort<T>(a, n, cmp)` and binary search `sort_search<T>(a, n, key, cmp)` (index or `-1`) over a `*T` array; `cmp` is `fn(*T, *T)->int` (negative / zero / positive); `List_sort_by(&list, cmp)` sorts a `List<T>` in place |
| `stdlib/url.esk`      | RFC 3986 percent-encoding: `url_encode`, `url_decode` (and `url_decode_range`), plus `url_query_get(query, key, &out)` for `a=1&b=2` query strings (keys and values are decoded, `+` to a space); `url_parse(text, &u)` into a `Url` (`scheme`, `host`, `port`, `path`, `query`), `Url_resolve(&base, ref, &u)` (RFC 3986 reference resolution), `Url_to_string`, `Url_target`, `Url_host_port`, `url_default_port`, `url_query_add(&text, key, value)` (see [HTTP client](#http-client-fetch)) |
| `stdlib/uuid.esk`     | `uuid_v4(&rng, &out)`: an RFC 4122 version-4 UUID string (`xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx`) drawn from a `<random>` `Rng` |
| `stdlib/env.esk`      | `env_get`, `env_has`, `env_get_or`, `env_get_int`, `env_set`, `env_unset`, `env_list` (process environment; CLI args come from `main`'s `argc`/`argv`) |
| `stdlib/process.esk`  | Child processes: `process_run` (capture stdout/stderr into a `ProcessResult`, with `ProcessOptions` for stdin text, cwd, extra environment, a timeout), `sh`/`sh_with` (through `/bin/sh -c` or `cmd.exe /c`), `process_spawn` with `Process_write_stdin`/`_close_stdin`/`_read`/`_read_all`/`_wait`/`_kill`/`_free`, `process_exit`, `process_pid`, `process_last_error`; see [Processes](#processes-process) |
| `stdlib/dns.esk`      | `dns_resolve(host, &list)` (IPv4 and IPv6 addresses as numeric strings, through `getaddrinfo`), `dns_free_list`, `dns_last_error` |
| `stdlib/fetch.esk`    | HTTP/1.1 client for `http://` and `https://` URLs: `fetch`, `fetch_with` with `FetchOptions` (`FetchOptions_init`, `_method`, `_header`, `_body_text`, `_body_bytes`, `_body_json`, `_free`), `fetch_async` over an `EventLoop`; `Response` (`Response_header`, `Response_text`, `Response_bytes`, `Response_json`, `Response_ok`, `Response_free`); `fetch_last_error`; the incremental response parser `FetchParser` (see [HTTP client](#http-client-fetch)) |
| `stdlib/fetch_tls.esk`| The HTTPS side of `<fetch>`: an OpenSSL client loaded at run time (`dlopen`/`LoadLibraryA`), `fetch_tls_available` |
| `stdlib/base64.esk`   | `base64_encode` / `base64_decode` over byte buffers (decoding rejects bad padding and truncated input), plus `base64_encoded_len` / `base64_decoded_len` and the `base64_value` / `base64_digit` primitives |
| `stdlib/bytes.esk`    | `Bytes`, a growable, binary-safe byte buffer (`*uint8` + length; embedded NULs survive, unlike `String`): `Bytes_init`/`_free`/`_push`/`_append`/`_append_raw`/`_slice` (non-owning view)/`_eq`/`_from_str`/`_cstr`, plus `Bytes_from_base64`/`Bytes_to_base64` |
| `stdlib/path.esk`     | Unix path manipulation: `path_join`, `path_basename`, `path_dirname`, `path_extension`, `path_is_absolute` |
| `stdlib/http.esk`     | HTTP/1.1: `HttpRequest` + `HttpRequest_parse`/`_parse_status`/`_header` (RFC 9112 framing, the same as `http_recv`: `_parse_status` is 0 for a complete request, -1 when more bytes are needed, else the HTTP error status), `HttpResponse` + `HttpResponse_header`/`_set_body`/`_render`/`_render_head` (a HEAD response: Content-Length, no body; a 1xx, 204 or 304 has neither; `HttpResponse_header` returns 1, or 0 and adds nothing for a name that is not a token or a value with a CR or LF, which would inject a header line; a Content-Length the handler sets is not sent next to the automatic one, only a 1xx, 204 or 304 keeps it), and a threaded worker pool `http_serve(port, nworkers, handler)` where `handler` is `fn(HttpRequest*, HttpResponse*)->void` (`http_serve_with(port, nworkers, handler, lim)` takes an `HttpLimits`); it reads each request until complete (`HttpConnBuf_feed`, incremental: the head is parsed once and a chunked body decoded as it arrives), answering a bad one 400, 413, 501 or 505 without the handler (limits: a head over `HTTP_SERVE_MAX_HEAD`, 64 KiB, is 400, a body over `HTTP_SERVE_MAX_BODY`, 1 MiB, 413). `HttpLimits` (`HttpLimits_init` / `http_limits()` give the defaults) holds every server's timeouts in ms, 0 turning one off: `header_ms` (`HTTP_HEADER_TIMEOUT_MS`, 10 s: the whole head; trickled bytes do not extend it), `body_ms` (`HTTP_BODY_TIMEOUT_MS`, 60 s: the body once the head is in), `idle_ms` (`HTTP_IDLE_TIMEOUT_MS`, 60 s: an HTTP/2 connection with no open stream), `write_ms` (`HTTP_WRITE_TIMEOUT_MS`, 30 s: the client must take the answer; an HTTP/2 stream counts it from the last response data sent), `write_total_ms` (`HTTP_WRITE_TOTAL_MS`, 5 min: an HTTP/2 stream's whole response must be sent within it from when it is ready, however the peer paces its window updates; past it the stream is reset with CANCEL and its buffered response freed, the connection stays open), and `max_open` (`HTTP_MAX_OPEN_CONNS`, 1024: connections an async server holds at once; one past it is closed at once). A request cut by `header_ms` or `body_ms` after some of it arrived is answered 408; a peer that sent nothing is just closed. `http_serve` applies them with `SO_RCVTIMEO` (set to what is left of the deadline before each read) and `SO_SNDTIMEO`. `http_chunked_step` decodes a chunked body incrementally; a chunk-size or trailer line longer than `HTTP_CHUNK_LINE_MAX` (8192 bytes, CRLF included) is malformed however it was split into reads. The servers and `http_recv` drop the chunked input they have decoded, so their buffer holds the head and one unfinished chunk or line, and they reject a trailer section longer than `HTTP_CHUNK_TRAILER_MAX` (64 KiB) with 400. Plus a binary-safe full-body reader `HttpReq` + `http_recv` (loops until the Content-Length body arrives, or decodes a chunked body, into a `*uint8` body; RFC 9112 framing: `Transfer-Encoding` with `Content-Length`, a missing, repeated or invalid `Host` (not uri-host [":" port]), whitespace before a header colon, an obsolete line fold or a bad version is 400, a coding other than `chunked` 501), `HttpReq_header`, `http_reply`, `http_reply_error`: for uploads a single-recv String body would corrupt binary bytes |
| `stdlib/multipart.esk`| Extract a named part from a `multipart/form-data` body over raw bytes: `multipart_boundary(ct, out)` and `multipart_part(body, len, boundary, name, *out_ptr, *out_len)` (the part whose `Content-Disposition` `name` parameter is `name`; returns a slice into the body) |
| `stdlib/map.esk`      | `Map<V>`, a string-keyed hash map (open addressing, linear probing, grows at 0.75 load): `Map_init`, `Map_at` (get-or-insert → `*V` slot, sets `*created`), `Map_get`, `Map_free`. Plus `HashMap<K,V>`, keyed on any value type via `hash`/`eq` function pointers passed to `HashMap_init` (built-in `int_hash`/`int_eq`, and `cstr_hash`/`cstr_eq` for `string` keys by content); same `_at`/`_get`/`_free` shape. Both have `_has`, `_get_or`, `_len`, `_remove`, `_keys`, `_values`, `_each`. `Set<T>` is a hash set over `HashMap`: `Set_init`, `Set_add`, `Set_has`, `Set_remove`, `Set_len`, `Set_each`, `Set_items`, `Set_free` (see [Map, HashMap and Set](#map-hashmap-and-set)) |
| `stdlib/threading.esk`| Synchronization over pthread: `Mutex` (`_init`/`_lock`/`_unlock`/`_destroy`), `Cond` (`_init`/`_wait`/`_signal`/`_broadcast`/`_destroy`), `Sem` (`_init`/`_wait`/`_post`/`_destroy`). Pairs with the `thread_create`/`thread_join` built-ins |
| `stdlib/eventloop.esk`| Readiness reactor over kqueue (macOS) / epoll (Linux): `EventLoop`, `el_new`, `EventLoop_add_read`, `EventLoop_add_write`, `EventLoop_del`, `EventLoop_run`, `EventLoop_stop`, `EventLoop_free`, plus a timer wheel (`EventLoop_add_timer`/`EventLoop_del_timer`). Callback is `fn(EventLoop*, int)->void` |
| `stdlib/atomic.esk`| Atomic intrinsics: `atomic_load`/`atomic_store`/`atomic_swap`/`atomic_cas` and `atomic_fetch_add`/`_sub`/`_and`/`_or`/`_xor` on an `int` cell, the same with a `64` suffix on an `int64` cell, `atomic_load_ptr`/`_store_ptr`/`_swap_ptr`/`_cas_ptr` on a pointer cell, `atomic_fence()`, and a `_relaxed` form of each but the fence; lowered to LLVM atomics with fixed orderings (6.4.1). Declared with the `intrinsic` qualifier |
| `stdlib/libc.esk`  | The C library for ported code, opt-in: the umbrella over the `stdlib/c/` modules (below) plus the C functions no other module declares: string.h (`strcpy`, `strncpy`, `strcat`, `strrchr`, `strspn`, `strdup`, ...), stdlib.h (`malloc`, `realloc`, `strtol`, `strtoll`, `atol`, `atoll`, `labs`, `llabs`, `qsort`, `bsearch`, `rand`, `system`, `atexit`, ...), stdio.h (`snprintf`, `sscanf`, `fputc`, `perror`, `remove`), ctype.h, the rest of libm (`sin` ... `tanh`, `exp`, `log`, `log2`, `log10`, `cbrt`, `hypot`, `round`, `trunc`, `fmin`, `fmax`, `float` versions), `getppid` and `access`, and `libc_errno()`/`libc_set_errno()` with `LIBC_E*` codes. Through `stdlib/c/` it also declares `strerror`, `strtod`, `atoi`, `atof`, `getenv`, `exit`, `vsnprintf`, `fopen`, `fgets`, `rename`, `time`, `raise`, `signal` and the POSIX unistd calls (`getpid`, `read`, `write`, `close`, `getcwd`, `unlink`, `sleep`, `usleep`). Imports `<mem>`, `<io>` and `<math>`. `long` is `c_long` from `<ctypes>` (32 bits on Windows) |
| `stdlib/json.esk`     | JSON builder + parser. Builder: `Json` + `Json_init`/`_free`/`_cstr`, `Json_obj_begin`/`_end`, `Json_arr_begin`/`_end`, `Json_key`, `Json_str`, `Json_int`, `Json_bool`, `Json_null` (auto separators). Parser (strict RFC 8259: exact literals, no trailing data, `\uXXXX` escapes decoded; returns `null` on malformed input): `json_parse(src) -> *JsonValue` + `JsonValue_kind`/`_len`/`_at`/`_get`/`_as_int`/`_as_double`/`_as_bool`/`_as_cstr`/`_free`. Documents: `JsonValue_object`/`_array`/`_string`/`_number`/`_int`/`_bool`/`_null`, `JsonValue_set`/`_push`/`_remove`, `JsonValue_stringify`/`_pretty`, `json_get(v, "a.b[0][\"k.x\"]")` |
| `stdlib/csv.esk`      | RFC 4180 CSV: `csv_parse`/`csv_parse_with`/`csv_parse_buf` into a `CsvTable` (`CsvTable_len`/`_row`/`_cell`/`_col`/`_get`/`_error`/`_write`/`_free`), `csv_write_row`/`csv_write_row_with` |
| `stdlib/dotenv.esk`   | `.env` files: `dotenv_load`, `dotenv_load_override`, `dotenv_parse` into a `Map<string>`, `dotenv_free`, `dotenv_error`/`dotenv_error_line` |
| `stdlib/hash.esk`     | Pure-Eskiu digests: `Sha256`, `Sha1`, `Md5`, `HmacSha256` (streaming `_init`/`_update`/`_update_str`/`_final`/`_final_hex`) and one-shot `hash_sha256`/`hash_sha256_hex`/`hash_sha256_str` (same for `hash_sha1`, `hash_md5`), `hash_hmac_sha256`/`hash_hmac_sha256_str`, `hash_hex_encode`/`hash_hex_decode`, constant-time `hash_equal`/`hash_equal_str` |
| `stdlib/args.esk`     | Command-line parsing: `ArgParser` (`_init`/`_parse`/`_parse_or_exit`/`_help`/`_usage`/`_error`/`_free`), declared with `ArgParser_flag`, `ArgParser_option`, `ArgParser_option_int`, `ArgParser_argument`, read with `ArgParser_get`, `ArgParser_get_int`, `ArgParser_get_flag`, `ArgParser_count`, `ArgParser_get_at`, `ArgParser_get_all`, `ArgParser_positional`, `ArgParser_rest` |
| `stdlib/term.esk`     | Terminal output: styles as `int64` bit sets (`TERM_BOLD`, `TERM_RED`, `TERM_BG_BLUE`, `term_fg256`, `term_fg_rgb`, ...), `term_style`/`term_style_fd`/`term_print`/`term_println`/`term_sgr`/`term_reset`/`term_strip`, `term_is_tty`, `term_width`, `term_color_enabled`, `term_set_color_mode` |
| `stdlib/log.esk`      | Leveled logging: `log_trace`/`log_debug`/`log_info`/`log_warn`/`log_error`/`log_at` (printf-style), `log_set_level` (or `LOG_LEVEL`), `log_set_file`/`log_set_stderr`, `log_set_time`, `log_set_color`, `log_freeze_time` |
| `stdlib/sysheap.esk`  | `Heap`, a general-purpose heap that `mmap`s OS pages and runs `FirstFit` over them, providing allocation with no libc `malloc` (suitable as a freestanding backend) |
| `stdlib/future.esk`   | The async runtime's `Future<T>` (the interface the compiler's async lowering targets): the `state`/`waker`/`on_drop` handshake, `future_new`/`Future_complete`/`Future_poll`/`Future_drop`/`Future_free`/`Future_free_polled` (the latter also frees the waker a hand-driven `Future_poll` installed), and the generic combinators `spawn<T>`, `select2<A,B>` (first of two), `join2<A,B>` (all of two) |
| `stdlib/executor.esk` | `Executor`, a thread that owns an event loop plus a thread-safe ready-queue of wakers woken through a self-pipe, so a waker (a coroutine resume) always runs on the executor's own thread: `executor_new`, `Executor_schedule`, `Executor_run`, `Executor_stop`, `Executor_free` |
| `stdlib/net_async.esk`| Leaf futures for non-blocking network I/O over `<eventloop>`: `net_set_nonblocking`, `net_read_async`, `net_write_async`, `net_accept_async`, each registers an fd and completes its `*Future<int>` when ready; `net_read_deadline_async`, `net_write_deadline_async` and `net_ready_deadline_async` race the same against a `timer_after` (via `select2`) and complete with `NET_TIMED_OUT` once the deadline passes |
| `stdlib/timer.esk`    | `timer_after(lp, ms)`, a `*Future<int>` that completes once `ms` of monotonic time elapses, driven by the loop's timer wheel; combine with a read for a real timeout |
| `stdlib/channel.esk`  | `Chan<T>`, an async message channel over the Future runtime: `chan_new<T>(cap)`, `Chan_send`, `Chan_recv` (a `*Future<T>` that completes with the next item), and `Chan_free` |
| `stdlib/either.esk`   | The standard sum types `Option<T>` and `Either<A,B>` (generic algebraic enums) plus helpers (`Option_is_some`/`Option_unwrap_or`, …) |
| `stdlib/futureval.esk`| Value-returning future combinators: `select2v<A,B>` resolves to the winner's value wrapped in `Either<A,B>`; `join2v<A,B>` resolves to a `Pair<A,B>` of both values |
| `stdlib/http_async.esk`| Non-blocking concurrent HTTP/1.1 server built on the event loop's accept loop: `http_serve_async` (and `http_serve_async_with(lp, fd, handler, max_conns, lim)` for other `HttpLimits`; the reads and the write race a timer, and at most `lim.max_open` connections are open at once), with the same `fn(HttpRequest*, HttpResponse*)->void` handler interface as `<http>`. Each connection socket is non-blocking and the answer is written with `net_write_async`, so a client that does not read parks only its own connection |
| `stdlib/http2.esk`    | HTTP/2 (RFC 7540) wire layer: the 9-byte frame header codec, frame-type/flag constants, the connection lifecycle (`H2Conn`: SETTINGS exchange + ACK, PING/PONG, GOAWAY), the per-stream state machine and credit-based flow control (`H2Stream`), the HEADERS/DATA/WINDOW_UPDATE/RST_STREAM codecs, and async frame I/O |
| `stdlib/hpack.esk`    | HPACK (RFC 7541) header compression: prefix-integer and string-literal coding, the 61-entry static table, a per-connection dynamic table, and the §6 field representations; the encoder picks the shorter of raw and Huffman |
| `stdlib/hpack_huffman.esk`| The HPACK Huffman code table (RFC 7541 Appendix B), generated from the RFC: `hpack_huff_code(i)` / `hpack_huff_len(i)`, used by `<hpack>` |
| `stdlib/http2_server.esk`| HTTP/2 (h2c, cleartext) server over the async event loop, `http2_serve_async` (`http2_serve_async_with` / `http2_serve_conn_async_with` take an `HttpLimits`: a connection without the preface and SETTINGS by `header_ms` is closed, one with no open stream gets GOAWAY NO_ERROR after `idle_ms` (PING does not extend it), a stream must be received within `body_ms` and its response taken within `write_ms` of the last data sent and within `write_total_ms` in all (a stream past `write_total_ms` is reset with CANCEL, `H2Server_expire`); the transport reads with `H2Server_deadline` after `H2Server_tick` and on a timeout calls `H2Server_timed_out_with`). The protocol is the transport-agnostic `H2Server` engine (also used by `<tls>`): multiplexed streams, flow control in both directions, padding, CONTINUATION, and RFC 9113 connection/stream errors; a completed request is HPACK-decoded into an `HttpRequest` and the `HttpResponse` goes back as a HEADERS frame plus flow-controlled DATA frames. Request bodies are bounded: at most `s.max_body` bytes per stream (default `HTTP_SERVE_MAX_BODY`, 1 MiB) and `s.max_buffered` across a connection's streams (default `H2_MAX_BUFFERED`, 4 MiB), both settable after `H2Server_init`; a request past either, or with a content-length past `max_body`, is answered 413 without the handler and its stream reset with NO_ERROR. A request's decoded header list is bounded too: more than `H2_MAX_HEADERS` (64) fields, more than `H2_MAX_HEADER_LIST` bytes (64 KiB, counted as `SETTINGS_MAX_HEADER_LIST_SIZE` counts them: name + value + 32 per field, and advertised in the server's SETTINGS), or more than `s.max_buffered` leaves (the open streams' header lists count toward it with their bodies) is answered 431 without the handler. The HTTP/1.1 Host rules apply: a host field or `:authority` that is not uri-host [":" port] is 400, and two host fields or a host that differs from `:authority` (ignoring case) is malformed (RST_STREAM PROTOCOL_ERROR); without a host field the handler sees `:authority` as its Host header. A response header line with a non-token name or an LF or NUL in its value is not sent |
| `stdlib/tls.esk`      | TLS for HTTP/2 over OpenSSL (libssl) with ALPN negotiating `"h2"`: a server `SSL_CTX` that loads a cert/key and selects `"h2"`, plus blocking (`http2_tls_serve_conn`) and async (`http2_tls_serve_async`) h2-over-TLS servers that run the `<http2>` protocol over the encrypted stream, with the h2c server's deadlines (`http2_tls_serve_conn_with`, `http2_tls_serve_async_with` take an `HttpLimits`; the handshake counts toward `header_ms`, and `tls_accept` bounds a blocking handshake too, `tls_accept_with` with other limits). The blocking calls (`tls_accept_with`, `tls_read_full_deadline`, `tls_write_all_deadline`, `http2_tls_serve_conn_with`) run the socket non-blocking and wait with `poll` on what is left of the deadline, so a peer that trickles bytes inside one TLS record cannot stretch it (a socket timeout restarts with every byte OpenSSL's record loop reads); `tls_accept_with` leaves the socket non-blocking |

**C library declarations (`stdlib/c/`).** Each C library function the stdlib calls is declared once, in an internal module per C header that the stdlib modules import (`import <c/unistd>;`, resolved to `stdlib/c/unistd.esk`): `<c/string>` (string.h: `memcpy` ... `strstr`, `strerror`), `<c/stdlib>` (`atoi`, `atof`, `strtod`, `getenv`, `exit`), `<c/stdio>` (the `FILE*` functions and `vsnprintf`), `<c/unistd>` (POSIX unistd.h and fcntl.h: `close`, `read`, `write`, `pipe`, `dup2`, `isatty`, `open`, `fcntl`, ...), `<c/time>` (`time`, `sleep`, `usleep`), `<c/signal>`, `<c/errno>` (the per-platform errno cell: `_errno`, `__error`, `__errno` or `__errno_location`, read with `_c_errno()`), `<c/poll>` (`poll` with each platform's `nfds_t`, `WSAPoll`, and `_c_poll(fds, n, ms)`), `<c/socket>` (BSD sockets and Winsock) and `<c/windows>` (the Win32 calls several modules share). Each declaration spells the C types with `<ctypes>` (`size_t`, `ssize_t`, `c_long`) and the platform `#ifdef`s. The `c/` modules are not a public API: a program reaches these functions through the public modules (`<mem>`, `<io>`, `<fs>`, `<net>`, ...) or `<libc>`. A program that declares one of them itself must spell the same types, as with any extern declared twice. The functions that only `<libc>` offers stay in `libc.esk`, so that importing `<mem>` or `<net>` does not claim names such as `strcpy`, `snprintf`, `qsort` or `log`.

Modules built around a struct name their operations `Type_method`, so they can also be called with dot syntax (`rng.next()` calls `Rng_next(&rng)`). There is no `Type::method` syntax: `Type_method` is the function's name and its linker symbol (no name mangling), and dot syntax is the way to call it as a method. A keyword after `.` is a member name, so `j.int(5)` and `j.bool(1)` call `<json>`'s `Json_int` and `Json_bool`. Functions that create a value keep a lowercase module name (`el_new`, `executor_new`, `chan_new`, `regex_compile`).

### Result<T,E>

```eskiu
import <result>;
import <io>;

Result<int, string> divide(int a, int b) {
    if (b == 0) return Err<int, string>("division by zero");
    return Ok<int, string>(a / b);
}

int main() {
    let r: Result<int, string> = divide(10, 2);
    if (r.ok) {
        printf("result: %d\n", r.value);
    } else {
        printf("error: %s\n", r.error);
    }
    return 0;
}
```

### List<T>

```eskiu
import <list>;
import <io>;

int main() {
    let items: List<int>;
    List_init(&items, 4);
    List_push(&items, 10);
    List_push(&items, 20);
    List_push(&items, 30);
    printf("len: %d\n", List_len(&items));
    printf("item[1]: %d\n", List_get(&items, 1));
    List_free(&items);
    return 0;
}
```

### String

```eskiu
import <string>;
import <io>;

int main() {
    let s: String;
    String_from(&s, "Hello");
    String_append(&s, ", world!");
    printf("%s\n", String_cstr(&s));   // Hello, world!
    String_free(&s);
    return 0;
}
```

`String` is a growable, owned, NUL-terminated buffer. Beyond append/concat it
offers per-character access and mutation (`String_char_at`, `String_set`,
`String_push`), comparison (`String_eq`, `String_eq_cstr`, and the operators `==` / `!=`
between two Strings or a String and a C string, which compare text), search
(`String_index_of`), `String_substring`, `String_reverse`, `String_clear`, and
integer conversion (`String_from_int`, `String_to_int`):

```eskiu
let n: String;
String_init(&n, 8);
String_from_int(&n, -2026);            // "-2026"
printf("%d\n", String_to_int(&n));     // -2026
String_free(&n);
```

### String scripting helpers

These follow the stdlib ownership rule: a `String* out` is cleared and refilled
(the caller initialised it and frees it), a `self` String is edited in place, and
split results are `List<String>`s of owned Strings released with
`String_split_free`. Lengths, widths and indices count bytes; case mapping
changes ASCII letters only and leaves every other byte (UTF-8 included) as is.

| Function | Behavior |
|---|---|
| `int64 String_find(String* self, string needle, int64 from)` | First index at or after `from`, or `-1`; an empty needle matches at `from` |
| `int String_contains(String* self, string needle)` | 1 or 0 |
| `int String_replace(String* self, string from, string to)` | Replace the first occurrence in place; 1 if replaced. An empty `from` replaces nothing |
| `int64 String_replace_all(String* self, string from, string to)` | Replace every non-overlapping occurrence, left to right (`"aaa"`, `"aa"` to `"b"` gives `"ba"`); returns the count |
| `void String_split_str(String* self, string sep, List<String>* out)` | Split on a string; separators at the ends or side by side give empty parts, an empty `sep` gives one part |
| `void String_lines(String* self, List<String>* out)` | Split on `\n` or `\r\n`; a final terminator adds no empty line, an empty String has none |
| `void List_join<T>(List<T>* self, string sep, String* out)` | Join the C strings of a `List<string>` into `out`: `parts.join(sep, &out)` |
| `void String_join(String* self, List<String>* parts, string sep)` | Join Strings (the inverse of a split) into `self` |
| `void String_lower(String* self)`, `void String_upper(String* self)` | ASCII case mapping in place |
| `void String_repeat(String* self, int64 n)` | `n` copies of itself; `n <= 0` empties it |
| `void String_pad_left(String* self, int64 width, char fill)`, `String_pad_right` | Pad to `width` bytes; a longer String is unchanged |
| `double String_to_double(String* self)` | Leading number in C `strtod` syntax, 0.0 if none |
| `int String_parse_double(String* self, *double out)` | The whole String (surrounding whitespace allowed) as one number: 1 and `*out` set, or 0 |
| `void String_from_double(String* self, double d)` | Shortest of `%.15g` / `%.17g` that reads back as `d` (`0.1`, `3`, `1e+20`) |
| `void String_format(String* self, string fmt, ...)` | `printf`-style formatting into `self` (cleared first), through the C library's `vsnprintf` (C rules: `%lld` for `int64`, `s.cstr()` for a String) |
| `void String_appendf(String* self, string fmt, ...)` | The same, appending |
| `void String_reserve(String* self, int64 need)` | Grow the buffer to at least `need` bytes |

```eskiu
let s: String;  s.from("a=1;b=2;;c=3");
let parts: List<String>;  List_init<String>(&parts, 4);
s.split_str(";", &parts);                   // [a=1] [b=2] [] [c=3]
String_split_free(&parts);
s.replace_all(";", ", ");                   // "a=1, b=2, , c=3"
let out: String;  out.init(16);
String_format(&out, "%s has %d bytes", s.cstr(), (int)s.len);
out.free();
s.free();
```

### List helpers

Predicates and mappers take the element by value (`fn(T)->bool`, `fn(T)->U`,
`fn(A, T)->A`), so a lambda fits directly. Equality takes two pointers and
returns 1 or 0 (`fn(*T, *T)->int`), the shape `HashMap` and `<sort>` use, so
`int_eq` from `<map>` works. A function that builds a list appends to an `out`
List the caller initialised. Elements are copied by value.

| Function | Behavior |
|---|---|
| `List_map<T,U>(self, fn(T)->U f, List<U>* out)` | Append `f(x)` for each element |
| `List_filter<T>(self, fn(T)->bool keep, List<T>* out)` | Append the elements `keep` accepts |
| `A List_reduce<T,A>(self, A init, fn(A, T)->A f)` | Left fold |
| `int List_find<T>(self, fn(T)->bool pred)` | Index of the first match, or `-1` |
| `int List_any<T>`, `int List_all<T>(self, fn(T)->bool pred)` | 1 or 0; `all` of an empty list is 1 |
| `int List_index_of<T>(self, T item, fn(*T,*T)->int eq)`, `int List_contains<T>(...)` | Index or `-1`; 1 or 0 |
| `void List_reverse<T>(self)` | In place |
| `void List_slice<T>(self, int start, int end, List<T>* out)` | Append `[start, end)`, clamped to the list |
| `void List_unique<T>(self, fn(*T,*T)->int eq)` | Keep the first of each run of equal elements, in place (O(n^2)) |
| `void List_clear<T>(self)` | Empty it, keeping the buffer |
| `T List_pop<T>(self)` | Remove and return the last element (a zeroed `T` when empty) |
| `void List_insert<T>(self, int i, T item)` | Insert before `i` (clamped) |
| `void List_sort_by<T>(self, fn(*T,*T)->int cmp)` | In `<sort>`: heapsort the list in place (not stable) |
| `void List_join<T>(self, string sep, String* out)` | In `<string>`: join a `List<string>` |

```eskiu
let nums: List<int>;  List_init<int>(&nums, 8);
for (i in 1..11) { nums.push(i); }
int limit = 4;
let big: List<int>;  List_init<int>(&big, 8);
nums.filter(bool(int x) { return x > limit; }, &big);          // 5..10
int sum = big.reduce(0, int(int acc, int x) { return acc + x; });  // 45
```

### Map, HashMap and Set

`Map<V>` and `HashMap<K,V>` gain `_has`, `_get_or(key, dflt)`, `_len`,
`_remove`, `_keys(List*)`, `_values(List*)` and `_each(fn(K, V)->void)`.
Iteration is in table order, which is unspecified; the callback of `_each` must
not insert or remove. `Map_remove` frees the map's copy of the key but not the
value. Removal uses backward-shift deletion (the later entries of the probe run
move back, no tombstones), so a slot pointer from `_at` is invalid after a
remove as well as after a growing insert. The strings `Map_keys` returns are the
map's own copies, valid until their entry is removed or the map freed.

`Set<T>` is a hash set over `HashMap<T, uint8>`, initialised with the same
`hash`/`eq` callbacks (`int_hash`/`int_eq`, `cstr_hash`/`cstr_eq` for `string`
elements by content, which stores the pointer and not a copy):

```eskiu
let seen: Set<string>;  Set_init<string>(&seen, 16, cstr_hash, cstr_eq);
seen.add("a");  seen.add("b");
int fresh = seen.add("a");                // 0: already there
printf("%d %d\n", seen.len(), seen.has("b"));   // 2 1
seen.remove("a");
seen.free();
```

### Networking (`<net>`)

`<net>` wraps the POSIX BSD socket API for TCP. The `net_*` helpers cover the
common path; the raw `extern`s (`socket`, `bind`, …) and a portable
`sockaddr_in` are also exported for anything more specialised. It is a pure
stdlib module. Sockets need no compiler support beyond the C FFI.

```eskiu
import <net>;
import <mem>;
import <io>;

int main() {
    int fd = net_tcp_listen(8080);            // bind + listen on 0.0.0.0:8080
    if (fd < 0) { return 1; }
    printf("listening on :8080\n");

    *uint8 req = alloc<uint8>(4096);
    while (1) {
        int c = net_accept(fd);               // blocking accept
        if (c < 0) { continue; }
        net_recv(c, (*void)req, 4096);
        net_send_str(c,
            "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nHello from Eskiu.\n");
        net_close(c);
    }
    free(req);
    return 0;
}
```

| Helper | Effect |
|---|---|
| `net_tcp_listen(port) -> int` | Create a socket, set `SO_REUSEADDR`, bind to `0.0.0.0:port`, listen. Returns the fd or `-1` |
| `net_accept(fd) -> int` | Accept the next connection; returns the connection fd or `-1` |
| `net_tcp_connect(host, port) -> int` | Connect to a dotted IPv4 host (`"127.0.0.1"`, used as is) or a host name or IPv6 address (`"localhost"`, `"::1"`), resolved with `<dns>` and tried address by address until one connects; returns fd or `-1` |
| `net_send(fd, buf, n)` / `net_recv(fd, buf, n)` | Send / receive raw bytes (`int64` count). A send to a closed peer returns `-1` (`EPIPE`); it never raises `SIGPIPE` |
| `net_send_str(fd, s)` | Send a C string (length via `strlen`) |
| `net_close(fd)` | Close a socket |
| `net_set_recv_timeout(fd, ms)` / `net_set_send_timeout(fd, ms)` / `net_set_timeouts(fd, recv_ms, send_ms)` | Bound a blocking `recv` / `send` (`SO_RCVTIMEO` / `SO_SNDTIMEO`; 0 waits forever). One that runs out returns `-1` with `net_would_block()` 1 |
| `net_deadline(ms) -> int64` | An absolute monotonic deadline `ms` from now (0 for none), for the `*_deadline` reads and writes, which return `NET_TIMED_OUT` (-3) once it passes |
| `net_wait_ready(fd, mode, deadline) -> int` | Wait until `fd` is readable (`mode` 0) or writable (1) by `deadline` (0 = none), with `poll` (`WSAPoll` on Windows): 0 when ready (an error or hang-up counts, for the next call to report), `NET_TIMED_OUT` once the deadline passed, `-1` if the wait failed |

`sockaddr_in` differs between macOS and Linux; `<net>` selects the right layout
at compile time using the predefined `__APPLE__` / `__linux__` macro (see §18).
A concurrent server combines `<net>` with `thread_create`, handing each
accepted connection to a worker (see §6.5 for passing a function as a value).
Complete programs are in `examples/http_server.esk` and
`examples/tcp_echo_server.esk`.

The HTTP servers (`http_serve`, `http_serve_async`, `http2_serve_async` and the TLS
servers) bound request heads, bodies and header lists, and they disconnect slow clients
under `<http>`'s `HttpLimits`: a request head (or the HTTP/2 preface and SETTINGS) must
arrive within `HTTP_HEADER_TIMEOUT_MS` (10 s) and a body within `HTTP_BODY_TIMEOUT_MS`
(60 s), an HTTP/2 connection with no open stream is closed with GOAWAY after
`HTTP_IDLE_TIMEOUT_MS` (60 s), a client must take an answer within
`HTTP_WRITE_TIMEOUT_MS` (30 s), and the async servers hold at most
`HTTP_MAX_OPEN_CONNS` (1024) connections at once. The `*_with` variants take other
limits.


### Files and directories (`<fs>`)

```eskiu
import <fs>;
import <io>;

int main() {
    fs_mkdir_p("out/logs");
    fs_write_text("out/logs/today.txt", "started\n");
    fs_append_text("out/logs/today.txt", "done\n");

    let found: List<String>;  List_init<String>(&found, 8);
    fs_glob("out/**/*.txt", &found);              // sorted: out/logs/today.txt
    fs_list_free(&found);

    fs_walk("out", void(string path, bool is_dir) {
        printf("%s%s\n", path, is_dir ? "/" : "");
    });

    let st: FileStat;
    if (fs_stat("out/missing.txt", &st) != 0) {
        let err: String;  err.init(32);
        fs_last_error(&err);                      // "No such file or directory"
        printf("%s\n", err.data);
        err.free();
    }
    return fs_remove_all("out");
}
```

Conventions: an action returns 0 on success and -1 on failure, and `fs_last_error(&s)`
(a message) or `fs_errno()` (the OS code: `errno`, or `GetLastError` on Windows)
describes the last failing call in the process (`fs_errno_missing()` tests for "no such
file"). A query (`fs_exists`, `fs_is_dir`, `fs_is_file`, `fs_match`) returns 1 or 0. A
function that fills a `String` clears it first; one that fills a `List<String>` appends
heap Strings to it, freed with `fs_list_free`. Paths are UTF-8 on every platform (UTF-16
on Windows underneath). The older `fs_write_all` keeps its 1/0 result.

| Function | Effect |
|---|---|
| `fs_read_text(path, &s)` / `fs_write_text(path, text)` / `fs_append_text(path, text)` | Whole-file text I/O (the read is binary-safe: NULs are kept and counted in `s.len`) |
| `fs_read_lines(path, &list)` | Each line without its `\n` or `\r\n` |
| `fs_stat(path, &st)` / `fs_lstat(path, &st)` | `FileStat {size, mtime, mode, is_dir, is_file, is_symlink}`: `mtime` in epoch seconds, `mode` the permission bits; `fs_lstat` does not follow a link |
| `fs_exists`, `fs_is_dir`, `fs_is_file` | 1 or 0, links followed |
| `fs_mkdir(path)` / `fs_mkdir_p(path)` | One directory / every missing parent (an existing directory is success) |
| `fs_remove(path)` / `fs_remove_all(path)` | A file or empty directory / a whole tree (links removed, never followed; a missing path is success) |
| `fs_rename(from, to)` / `fs_copy(from, to)` | Move (replacing a file at `to`) / copy a file's bytes and permission bits |
| `fs_list_dir(path, &list)` | The names in a directory, without `.` and `..`, sorted bytewise |
| `fs_walk(root, visit)` | `visit(path, is_dir)` for every entry under `root`, depth first and sorted; links are reported but not followed, so a link loop cannot recurse |
| `fs_glob(pattern, &list)` | Paths matching a pattern (sorted, no duplicates; returns the count): each `/` component is an `fs_match` pattern and `**` matches any number of directories; wildcards skip names starting with `.` unless the component starts with `.` |
| `fs_match(pattern, name)` | Shell matching of one name: `*`, `?`, `[abc]`, `[a-z]`, `[!a-z]`, `\` escapes |
| `fs_cwd(&s)` / `fs_chdir(path)` | The current directory |
| `fs_temp_file(prefix, &s)` / `fs_temp_dir(prefix, &s)` | Create a unique file / directory in the temporary directory (`$TMPDIR` or `/tmp`, `GetTempPathW` on Windows; `fs_temp_base(&s)` names it); the caller removes it |

### Processes (`<process>`)

```eskiu
import <process>;
import <io>;

int main() {
    let r: ProcessResult;
    sh("uname -s", &r);                           // through /bin/sh -c (cmd.exe /c on Windows)
    printf("exit %d: %s", r.exit_code, r.stdout.data);
    ProcessResult_free(&r);

    let argv: List<string>;  List_init<string>(&argv, 4);
    argv.push("grep");  argv.push("-c");  argv.push("x");
    let opts: ProcessOptions;              // zero: the defaults
    opts.stdin_text = "x\ny\nx\n";
    opts.timeout_ms = 5000;
    if (process_run(&argv, &opts, &r) != 0) {   // -1: could not start
        let err: String;  err.init(32);
        process_last_error(&err);
        printf("%s\n", err.data);
        err.free();
    }
    printf("%s", r.stdout.data);                  // 2
    ProcessResult_free(&r);
    List_free<string>(&argv);
    return 0;
}
```

`process_run(argv, opts, &r)` runs `argv[0]` (found through `PATH`) with the arguments
as given (no shell, no quoting), writes `opts.stdin_text` to its stdin and collects
stdout and stderr into `r.stdout` and `r.stderr`, reading both pipes together so that a
child filling one while the other is unread cannot deadlock. `opts` may be `null`. It
returns 0 when the program ran (whatever its exit code) and -1 when it could not start
(not found, a bad `cwd`; `process_last_error` says why, `r.exit_code` is -1). `r.exit_code`
is the exit status, or 128+N when signal N ended the child. With `timeout_ms` the child is
killed when the time runs out (on POSIX its whole process group, so the commands a shell
started die too), `r.timed_out` is set and `exit_code` is 137. `ProcessOptions` fields:
`stdin_text`, `cwd`, `env` (a `List<string>*` of extra `"NAME=value"` entries replacing
same-named ones), `timeout_ms`, and the flags `inherit_stdin`, `inherit_stdout`,
`inherit_stderr` (use the parent's stream instead of a pipe; without `stdin_text` or
`inherit_stdin` the child's stdin is empty) and `merge_stderr` (`2>&1`). When the child
inherits stdout or stderr, `process_run`, `sh_with` and `process_spawn` flush the
parent's stdio buffers first, so what the parent printed before comes out first. Free a
result with `ProcessResult_free`.

`process_spawn(argv, opts, &p)` starts the child and returns at once. Its stdin is a
pipe unless `inherit_stdin` (`Process_write_stdin`, `Process_close_stdin`), its output
pipes are read with `Process_read(&p, 1 or 2, &s)` (one read, up to 64 KiB; 0 at the end)
or `Process_read_all`; `Process_wait` closes stdin, reaps the child and returns its exit
code, `Process_kill` ends it, `Process_free` closes the pipes. Reading one pipe to its end
while the child fills the other can block, so set `merge_stderr` or `inherit_stderr` when
the child writes much to both. `process_exit(code)` ends the program, `process_pid()` is
its process id.

On POSIX the child is started with `fork` and `execvp` (only async-signal-safe calls in
between) and `SIGPIPE` is restored to its default in it. On Windows `CreateProcessW` runs
the command line built from `argv` with the C runtime's quoting rules, and threads drain
the pipes; with a timeout the child runs in a Job object, so the timeout ends the processes
it started too.

### Environment (`<env>`)

`env_get(name)` (null if unset), `env_has`, `env_get_or(name, fallback)` and
`env_get_int(name, fallback)` read the environment. `env_set(name, value)` and
`env_unset(name)` change it (0, or -1 for an empty name, one with `=`, or a failure), and
`env_list(&list)` appends every variable as a `"NAME=value"` String, sorted (free with
`fs_list_free`). Changes are inherited by processes started afterwards. On Windows the
C runtime and the Win32 environment are both updated, and an empty value removes the
variable.

### DNS (`<dns>`)

```eskiu
import <dns>;
import <io>;

int main() {
    let addrs: List<string>;  List_init<string>(&addrs, 4);
    if (dns_resolve("localhost", &addrs) < 0) { printf("%s\n", dns_last_error()); }
    for (i in 0..addrs.size) { printf("%s\n", List_get<string>(&addrs, i)); }  // ::1, 127.0.0.1
    dns_free_list(&addrs);
    return 0;
}
```

`dns_resolve(host, &list)` appends the host's IPv4 and IPv6 addresses as numeric
strings, in the resolver's order and without duplicates, and returns how many it added,
or -1 when the name does not resolve (`dns_last_error()` is the resolver's message). A
numeric host resolves to itself. The strings are heap copies: free the list with
`dns_free_list`. `<net>` imports `<dns>`, so `net_tcp_connect` accepts host names.

### HTTP client (`<fetch>`)

```eskiu
import <fetch>;
import <io>;

int main() {
    let o: FetchOptions;  FetchOptions_init(&o);
    defer FetchOptions_free(&o);
    FetchOptions_method(&o, "POST");
    FetchOptions_header(&o, "X-Request-Id", "42");
    let j: Json;  Json_init(&j);
    Json_obj_begin(&j);  Json_key(&j, "name");  Json_str(&j, "eskiu");  Json_obj_end(&j);
    FetchOptions_body_json(&o, &j);           // also sets Content-Type: application/json
    Json_free(&j);

    let r: Response;
    defer Response_free(&r);
    if (fetch_with("https://example.com/api/items", &o, &r) != 0) {
        printf("%s\n", fetch_last_error());
        return 1;
    }
    let ct: String;  String_init(&ct, 16);
    Response_header(&r, "content-type", &ct);  // any case
    printf("%d %s, %lld bytes\n", r.status, ct.cstr(), r.body.len);
    ct.free();
    return 0;
}
```

`fetch(url, &r)` (a GET with the default options) and `fetch_with(url, &o, &r)`
return 0 when a response arrived, whatever its status (`Response_ok` is 1 for 2xx), or
a negative code: `FETCH_ERR_URL` (an invalid URL or a scheme other than `http`/`https`),
`FETCH_ERR_DNS`, `FETCH_ERR_CONNECT` (no address accepted the connection),
`FETCH_ERR_TIMEOUT`, `FETCH_ERR_TLS` (handshake or certificate check),
`FETCH_ERR_IO`, `FETCH_ERR_RESPONSE` (malformed or cut short), `FETCH_ERR_REDIRECTS`,
`FETCH_ERR_TOO_LARGE`, `FETCH_ERR_REQUEST` (an invalid method or header) or
`FETCH_ERR_NO_TLS`. `fetch_last_error()` describes the last failure (one message for the
program, not per thread). Both initialize the `Response` themselves, so it is freed with
`Response_free` in every case; after an error its status is 0.

`Response` has `status`, `reason`, `headers` (`"Name: value\n"` lines as received),
`body` (a `Bytes`), `url` (the final URL) and `redirects`. `Response_header` looks a header
up by name in any case, `Response_text` returns the body as a NUL-terminated string,
`Response_bytes` a non-owning `Bytes` view, and `Response_json` the body parsed with
`json_parse` (null if it is not JSON; free it with `JsonValue_free`).

`FetchOptions_init` sets the defaults, which are plain fields to change afterwards:
`method` (`"GET"`), `timeout_ms` (30000, for the whole call including redirects; 0 for
none; the host name lookup is not bounded by it), `follow_redirects` (1),
`max_redirects` (10), `max_body` (64 MiB; a longer body is `FETCH_ERR_TOO_LARGE`),
`user_agent` (`"eskiu/0.12.0"`; empty sends none), `verify_tls` (1) and `ca_file` (a PEM
bundle trusted besides the system store). `FetchOptions_header(&o, name, value)` returns
0 and adds nothing when the name is not a token or the value holds a CR or LF; the
request then fails with `FETCH_ERR_REQUEST`, so a header can never be split into an
injected one. `url_parse` rejects control bytes in the URL, so the request target cannot
carry one either.

The request is HTTP/1.1 with `Host`, `User-Agent`, `Accept: */*` (unless the options set
them), the options' headers, `Content-Length` when there is a body or the method is
`POST`, `PUT` or `PATCH`, and `Connection: close`: one request per connection. `Host`,
`Content-Length`, `Connection` and `Transfer-Encoding` headers in the options are not
sent. The response body is read by chunked transfer coding, by `Content-Length`, or until
the server closes the connection; a body that ends early is `FETCH_ERR_RESPONSE`, and
bytes past a `Content-Length` are ignored. Interim `1xx` responses are skipped. Codings
are not decoded (no `Accept-Encoding` is sent).

Redirects (301, 302, 303, 307, 308 with a `Location`) are followed up to `max_redirects`
times; one more is `FETCH_ERR_REDIRECTS`. A relative `Location` is resolved against the
current URL with `Url_resolve`. After 301, 302 or 303 the request becomes a `GET` without
a body (a `HEAD` stays a `HEAD`); 307 and 308 repeat it unchanged. A redirect from
`https` to `http` is followed, as browsers and `curl -L` do. A redirect to another origin
(scheme, host or port) drops the `Authorization` and `Cookie` headers.

HTTPS goes through OpenSSL 3 or 1.1, which `<fetch_tls>` loads with `dlopen` (or
`LoadLibraryA`) on the first `https://` request instead of linking it, so programs that
import `<fetch>` link and run without OpenSSL, and only an `https://` request fails
(`FETCH_ERR_NO_TLS`) where it is missing. It looks for the file named by `ESKIU_LIBSSL`,
then `libssl.3.dylib` / `libssl.so.3` / `libssl-3-x64.dll` (and the 1.1 names) on the
library path, and on macOS also in Homebrew (`/opt/homebrew/opt/openssl@3/lib`,
`/usr/local/opt/openssl@3/lib`) and MacPorts; `fetch_tls_available()` reports whether it
was found. The client sends SNI, verifies the certificate against OpenSSL's default trust
store and checks the host name (`SSL_set1_host`); `verify_tls = 0` skips both, for local
tests only. On Windows OpenSSL's default store is usually empty, so a verifying request
without `ca_file` also trusts the certificates of the system "ROOT" store (read with
`CertOpenSystemStoreW` from crypt32 and added to the context's `X509_STORE`), the roots
Windows itself trusts. With `ca_file` set, only that file and OpenSSL's default store count.
Defining `ESKIU_FETCH_NO_TLS` before `import <fetch>` leaves HTTPS out of the program.

`fetch_async(lp, url, &o, &r)` runs the same request over an `<eventloop>` and returns a
`Future<int>` that completes with the same code. Connecting, the TLS handshake and the
reads and writes wait on the loop; the host name lookup blocks. The options are copied
when the call starts; `url` and `r` must stay valid until the future completes.

```eskiu
async int get_both(EventLoop* lp, FetchOptions* o) {
    let a: Response;
    let b: Response;
    Future<int>* fa = fetch_async(lp, "https://example.com/a", o, &a);
    Future<int>* fb = fetch_async(lp, "https://example.com/b", o, &b);
    int both = await join2(fa, fb);           // the two requests run at the same time
    int rc = fa.value != 0 ? fa.value : fb.value;
    Future_free<int>(fa);  Future_free<int>(fb);
    Response_free(&a);  Response_free(&b);
    return rc;
}
```

`FetchParser` is the response parser on its own: `FetchParser_init(&p, max_body,
is_head)`, then `FetchParser_feed(&p, &r, bytes, n)` as data arrives (1 when the response
is complete, 0 for more, or an error code) and `FetchParser_finish(&p, &r)` when the
connection closes.
---

### JSON documents (`<json>`)

```eskiu
import <json>;
import <io>;

int main() {
    *JsonValue doc = JsonValue_object();
    JsonValue_set(doc, "name", JsonValue_string("Eskiu"));
    *JsonValue tags = JsonValue_array();
    JsonValue_push(tags, JsonValue_string("systems"));
    JsonValue_push(tags, JsonValue_int(10));
    JsonValue_set(doc, "tags", tags);

    let out: String;  out.init(64);
    JsonValue_stringify(doc, &out);              // {"name":"Eskiu","tags":["systems",10]}
    printf("%s\n", out.data);
    JsonValue_pretty(doc, 2, &out);              // one member per line, 2 spaces per level
    printf("%s\n", JsonValue_as_cstr(json_get(doc, "tags[0]")));   // systems
    out.free();
    JsonValue_free(doc);
    return 0;
}
```

`JsonValue_object`, `_array`, `_string`, `_number`, `_int`, `_bool` and `_null` make
fresh values. `JsonValue_set(obj, key, val)` adds a member or replaces the one with that
key in place, `JsonValue_push(arr, val)` appends, and `JsonValue_remove(obj, key)` frees a
member (1 if it was there). A container owns its children: `set` and `push` take
ownership of `val` (which must not already be inside another container), replacing or
removing frees the old member, and `JsonValue_free` on the root frees the document. Both
return -1 and leave `val` with the caller when the target has the wrong kind.

`JsonValue_stringify(v, &out)` writes compact RFC 8259 text and `JsonValue_pretty(v,
indent, &out)` the indented form (`JSON.stringify(v, null, indent)` layout); `out` is
cleared first. Strings escape `"`, `\` and control bytes (a NUL is `\u0000`); numbers
use the shortest of `%.15g`/`%.16g`/`%.17g` that reads back as the same double, and NaN
or an infinity prints as `null`. Numbers are doubles, so integers beyond 2^53 lose
precision. The printer keeps its own stack, so a document of any depth prints.

`json_get(v, path)` follows a path of `name` / `.name` member steps, `[N]` array
indices and `["key"]` steps for keys that hold dots or brackets (`\"` and `\\` escape
inside them): `json_get(doc, "data.items[0].name")`. It returns a borrowed pointer, or
null when a step is missing, has the wrong kind, or the path is malformed.

### CSV (`<csv>`)

```eskiu
import <csv>;
import <io>;

int main() {
    let t: CsvTable;
    if (csv_parse("name,city\nAda,London\n\"Hopper, Grace\",\"New York\"\n", &t) != 0) {
        let msg: String;  msg.init(32);
        CsvTable_error(&t, &msg);                // such as "line 3, col 1: unterminated quoted field"
        printf("%s\n", msg.data);
        msg.free();
    }
    for (r in 1..CsvTable_len(&t)) { printf("%s\n", CsvTable_get(&t, r, "name")); }
    CsvTable_free(&t);

    let row: List<string>;  List_init<string>(&row, 2);
    row.push("a,b");  row.push("plain");
    let out: String;  out.init(32);
    csv_write_row(&row, &out);                   // "a,b",plain
    printf("%s", out.data);
    out.free();
    List_free<string>(&row);
    return 0;
}
```

`csv_parse(text, &t)` reads RFC 4180 CSV with `,` between fields; `csv_parse_with(text,
delim, &t)` takes another delimiter (`;`, `\t`, ...) and `csv_parse_buf(data, n, delim,
&t)` reads bytes that may hold NULs. Quoted fields may contain the delimiter, line breaks
and `""` (one quote); lines end with LF, CRLF or a lone CR; a final line ending adds no
row, while a blank line elsewhere is a row with one empty field. They return 0, or -1 for
an unterminated quoted field or text after a closing quote, with `CsvTable_error(&t,
&out)` giving `line L, col C: ...`. The parse initialises `t`; free it with
`CsvTable_free` in both cases.

Row indices count every row, so with a header the data starts at row 1.
`CsvTable_len`, `CsvTable_row(t, r)` (a `*List<String>`), `CsvTable_cell(t, r, c)`,
`CsvTable_col(t, "name")` (header index or -1) and `CsvTable_get(t, r, "name")` return
values borrowed from the table, or null when out of range. `csv_write_row(&fields,
&out)` and `csv_write_row_with(&fields, delim, &out)` append one row ending in `\n`,
quoting only fields that contain the delimiter, a quote, CR or LF; `CsvTable_write(t,
delim, &out)` writes a whole table (cleared first).

### `.env` files (`<dotenv>`)

```eskiu
import <dotenv>;
import <io>;

int main() {
    if (dotenv_load(".env") < 0) {
        let msg: String;  msg.init(64);
        dotenv_error(&msg);                      // such as "line 4: expected '=' after the variable name"
        printf("%s\n", msg.data);
        return 1;
    }
    printf("%s\n", env_get_or("DATABASE_URL", "unset"));
    return 0;
}
```

Each non-blank line is `KEY=VALUE`, optionally after `export `; `#` starts a comment
line. An unquoted value is trimmed and ends at a `#` that follows whitespace. Single
quotes keep the text as written; double quotes decode `\n`, `\t`, `\r`, `\"` and `\\`.
Quoted values may span lines. Names start with a letter or `_` and continue with
letters, digits, `_`, `.` and `-`; a later assignment wins.

`dotenv_load(path)` sets the file's variables that are not already set and returns how
many it set; `dotenv_load_override(path)` replaces existing ones too. On a read failure
or a syntax error they return -1 and set nothing; `dotenv_error(&out)` describes it
(`line N: ...` or `cannot read PATH: reason`) and `dotenv_error_line()` is the line (0
for a read failure). `dotenv_parse(text, &map)` fills an initialised `Map<string>`
without touching the environment and returns the number of assignments; the map owns
the value copies, so free it with `dotenv_free`.

### Hashes (`<hash>`)

```eskiu
import <hash>;
import <io>;

int main() {
    let hex: String;  hex.init(65);
    hash_sha256_str("abc", &hex);                // ba7816bf...f20015ad
    printf("%s\n", hex.data);

    let h: Sha256;  h.init();                    // streaming
    h.update_str("ab");  h.update_str("c");
    let digest: uint8[32];
    h.final(&digest[0]);

    hash_hmac_sha256_str("key", "The quick brown fox jumps over the lazy dog", &hex);
    printf("%d\n", hash_equal_str((string)hex.data, "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"));
    hex.free();
    return 0;
}
```

SHA-256, SHA-1 and MD5 are written in Eskiu (no OpenSSL). Each has a streaming type
(`Sha256`, `Sha1`, `Md5`) with `_init`, `_update(data, n)`, `_update_str(s)`,
`_final(out)` (the raw digest: `SHA256_SIZE` 32, `SHA1_SIZE` 20, `MD5_SIZE` 16 bytes into
a caller buffer) and `_final_hex(&out)`; a value is `init`ed again before reuse. The
one-shot forms are `hash_sha256(data, n, out)` (raw), `hash_sha256_hex(data, n, &out)` and
`hash_sha256_str(s, &out)` (lowercase hex), and the same for `hash_sha1` and `hash_md5`. `HmacSha256`
(`_init(key, klen)`, `_update`, `_final`) and `hash_hmac_sha256(key, klen, data, n, out)` /
`hash_hmac_sha256_str(key, msg, &out)` implement RFC 2104 (keys over 64 bytes are hashed
first). `hash_hex_encode(data, n, &out)` writes lowercase hex; `hash_hex_decode(s, out, cap)`
returns the byte count, or -1 for an odd length, a non-hex character or more than `cap`
bytes. `hash_equal(a, b, n)` and `hash_equal_str(a, b)` compare in time that depends
only on the length, for MACs and tokens. SHA-1 and MD5 are only for matching existing
checksums.

### Console input and output (`<io>`)

```eskiu
import <io>;

int main() {
    println("hello");                           // stdout
    println_err("warning: low disk");           // stderr, after flushing stdout
    let name: String;  name.init(32);
    if (input("name? ", &name) == 0) { printf("hi %s\n", name.cstr()); }
    while (read_line(&name) == 0) { println(name.cstr()); }
    name.free();
    return 0;
}
```

`print(s)` and `println(s)` write a `string` to stdout through C stdio, so they keep
their order with `printf`. `print_err(s)` and `println_err(s)` flush stdout, then write to
stderr. `read_line(&out)` reads one line of stdin into a `String` (cleared first) without
its `"\n"` or `"\r\n"`; it returns 0, or -1 at end of input when nothing was read (a last
line without a newline still returns 0). `input(prompt, &out)` prints the prompt, flushes,
and reads a line the same way. `read_all_stdin(&out)` reads the rest of stdin, binary safe
(`out.len` counts NUL bytes too; stdin is switched to binary mode on Windows), and returns
the byte count. All three read through the C `stdin` buffer that `scanf` and `getchar`
share. `io_stdin()`, `io_stdout()` and `io_stderr()` return the C `FILE*` streams for use
with the `<fs>` functions. (The names `eprint`/`eprintln` are left free: the self-hosted
compiler defines its own `eprint`.)

### Time formatting, local time and durations (`<time>`)

```eskiu
import <time>;
import <io>;

int main() {
    let s: String;  s.init(32);
    time_format(time_now_s(), "%a %F %T %Z (%z)", TIME_LOCAL, &s);
    int64 t = 0;
    time_parse("2026-10-11T10:30:05+02:00", "%FT%T%z", &t);   // 1791707405
    time_format_duration(3723000, &s);                              // "1h 2m 3s"
    let sw: Stopwatch;  sw.start();
    time_sleep_ms(10);
    printf("%lld ms\n", sw.elapsed_ms());
    s.free();
    return 0;
}
```

`time_format(t, fmt, zone, &out)` formats epoch seconds in `TIME_UTC` or `TIME_LOCAL` and
returns 0, or -1 when `fmt` has an unknown specifier (copied as written). The specifiers,
for both formatting and parsing, are `%Y %m %d %H %M %S %j` (zero-padded numbers),
`%a %A` and `%b %B` (English weekday and month names, short and full), `%Z` (zone name:
`UTC`, or the C library's local name), `%z` (`+hhmm`), `%s` (epoch seconds), `%F`
(`%Y-%m-%d`), `%T` (`%H:%M:%S`) and `%%`. Local time follows the system zone (`TZ` on
POSIX, read again on every call) through `localtime_r` / `_localtime64_s`;
`time_to_local(t, &dt)` fills a `DateTime` and returns the offset from UTC in seconds, and
`time_local_offset(t)` returns just the offset.

`time_parse(text, fmt, &t)` must match all of `text`; a space in `fmt` matches any run of
whitespace. Names match in any case, full or abbreviated; `%z` takes `Z`, `+hh`, `+hhmm`
or `+hh:mm` and `%Z` takes `UTC`, `GMT` or `Z`. Fields not given default to 1970-01-01
00:00:00, a date that does not exist (2023-02-29, month 13, hour 24) is an error (-1),
and the time is UTC unless an offset is parsed or `%s` sets the instant.
`time_parse_local` reads a time without an offset in the local zone.

`time_format_duration(ms, &out)` writes `1d 2h 3m 4s 5ms`, leaving out zero parts (`0ms` for
zero, a leading `-` when negative). A `Stopwatch` measures on the monotonic clock:
`start` (or restart), `elapsed_ms`, and `lap` (milliseconds since the previous lap or the
start).

### Terminal colors (`<term>`)

```eskiu
import <term>;

int main() {
    term_println("ok", TERM_BOLD | TERM_GREEN);
    let s: String;  s.init(64);
    term_style(&s, "error:", TERM_BOLD | TERM_RED);
    s.append(" not found");
    println(s.cstr());
    term_print("sunset\n", term_fg_rgb(255, 128, 0) | term_bg256(17));
    printf("%d columns\n", term_width());
    s.free();
    return 0;
}
```

A style is an `int64` combined with `|`: attributes `TERM_BOLD`, `TERM_DIM`,
`TERM_ITALIC`, `TERM_UNDERLINE`, `TERM_BLINK`, `TERM_REVERSE`, `TERM_HIDDEN`,
`TERM_STRIKE`; one foreground color (`TERM_BLACK` ... `TERM_WHITE`, `TERM_BRIGHT_*`,
`TERM_GRAY`, `term_fg256(n)`, `term_fg_rgb(r, g, b)`); one background color (`TERM_BG_*`,
`term_bg256(n)`, `term_bg_rgb(r, g, b)`). `TERM_PLAIN` is no style. `term_sgr(&out,
style)` appends the escape sequence and `term_reset(&out)` the reset, always.
`term_style(&out, text, style)` appends the text wrapped in them only when colors are
enabled for stdout (`term_style_fd(fd, ...)` for another stream), and
`term_print`/`term_println` print that way. `term_strip(&s)` removes escape sequences.

`term_color_enabled(fd)` decides: `FORCE_COLOR` set to anything but `""` or `"0"` turns
colors on; otherwise `NO_COLOR` set and not empty turns them off, as does `TERM=dumb`;
otherwise they are on when `term_is_tty(fd)`. `term_set_color_mode(TERM_ALWAYS)` /
`TERM_NEVER` / `TERM_AUTO` overrides those rules for the program. On Windows the
console's virtual terminal processing is enabled the first time colors are used on it.
`term_width()` is `COLUMNS` when it is a positive number, else the width of the terminal
behind stdout, stderr or stdin (`TIOCGWINSZ`, `GetConsoleScreenBufferInfo`), else 80.

### Logging (`<log>`)

```eskiu
import <log>;

int main() {
    log_info("listening on port %d", 8080);
    log_debug("hidden unless LOG_LEVEL=debug");
    log_set_level(LOG_DEBUG);
    log_set_time(LOG_TIME_LOCAL);
    log_warn("retry %d of %d", 1, 3);
    return 0;
}
```

This prints lines such as `2026-10-11T08:30:05.123Z INFO  listening on port 8080` to
stderr. The functions are `log_trace`, `log_debug`, `log_info`, `log_warn`, `log_error`
and `log_at(level, fmt, ...)`, printf-style with C `vsnprintf` rules. The minimum level is
the one given to `log_set_level` (`LOG_TRACE` ... `LOG_ERROR`, `LOG_OFF`; `-1` goes back to
the environment), else `LOG_LEVEL` (`trace`, `debug`, `info`, `warn`/`warning`, `error`,
`off`, any case, or `0`-`5`), else `LOG_INFO`; a message below it is not formatted.
`log_enabled(level)` and `log_level()` query it and `log_level_from_name` parses a name.
Timestamps are UTC with milliseconds by default; `log_set_time(LOG_TIME_LOCAL)` prints
the local time with its offset (`2026-10-11T04:30:05.123-04:00`) and `LOG_TIME_NONE` none.
`log_freeze_time(ms)` stamps every line with a fixed time (for tests) until
`log_unfreeze_time()`. `log_set_file(path)` appends to a file (0, or -1 if it cannot be
opened), flushed after every line; `log_set_stderr()` returns to stderr. On stderr the
level tag is colored through `<term>` when colors are enabled for stderr;
`log_set_color(TERM_ALWAYS / TERM_NEVER / TERM_AUTO)` changes that.

### Command-line arguments (`<args>`)

```eskiu
import <args>;

int main(int argc, string* argv) {
    let p: ArgParser;
    ArgParser_init(&p, "copy", "Copy files.");
    defer ArgParser_free(&p);
    ArgParser_flag(&p, "-v", "--verbose", "say more");
    ArgParser_option(&p, "-o", "--output", "DIR", ".", "where to copy");
    ArgParser_option_int(&p, "-j", "--jobs", "N", 1, "parallel copies");
    ArgParser_argument(&p, "files", "files to copy", ARGS_REQUIRED | ARGS_MANY);
    ArgParser_parse_or_exit(&p, argc, argv);
    for (i in 0..ArgParser_count(&p, "files")) {
        printf("%s -> %s\n", ArgParser_get_at(&p, "files", i), ArgParser_get(&p, "output"));
    }
    return 0;
}
```

`ArgParser_flag(p, short, long, help)`, `ArgParser_option(p, short, long, metavar, default, help)`
and `ArgParser_option_int(...)` (an `int` default; a value that is not an integer is a parse
error) declare options; either name may be `null`. `ArgParser_argument(p, name, help, mode)`
declares a positional argument: `ARGS_OPTIONAL`, `ARGS_REQUIRED`, and `ARGS_MANY` for the
last one (zero or more, one or more with `ARGS_REQUIRED`). A parser that declares no
arguments accepts any positionals; one that declares some rejects extras.

Accepted forms: `-v`, `--verbose`, `-o x`, `-ox`, `--output x`, `--output=x`, short
clusters (`-vo x`: an option in a cluster takes the rest of it or the next argument),
`--` (everything after it is positional, also kept for `ArgParser_rest`), and `-` as a
positional. An option's value is the next argument even when it starts with `-`; `-5` is a
positional unless a short option is a digit. A repeated option keeps all its values
(`ArgParser_get` returns the last, `ArgParser_get_all`/`ArgParser_get_at` every one) and a repeated flag
counts (`ArgParser_count`). `-h`/`--help` are automatic unless the program declares those
names.

`ArgParser_parse(p, argc, argv)` returns `ARGS_OK`, `ARGS_HELP` after printing the
generated help to stdout, or `ARGS_ERROR` after printing the usage line and `prog: error:
...` to stderr (the message is also `ArgParser_error(p)`). `ArgParser_parse_or_exit`
exits with 0 after the help and with 2 after an error. `ArgParser_help(p, &out)` and
`ArgParser_usage(p, &out)` produce the texts. Accessors take a long name without dashes
(`"output"`), a short name (`"o"` or `"-o"`) or an argument name: `ArgParser_get` (the value or
the default, `null` if neither), `ArgParser_get_int`, `ArgParser_get_flag`, `ArgParser_count`,
`ArgParser_get_at`, `ArgParser_get_all`, and `ArgParser_positional(p, i)` / `ArgParser_positional_count(p)`
for every positional in order. Returned strings point into `argv` or the defaults.

## 15. Inline Assembly

Eskiu supports inline assembly via the `asm(...)` statement, which lowers directly to an LLVM inline asm node.

### 15.1 Simple Form

```eskiu
asm("cli");         // disable interrupts (x86)
asm("sti");         // enable interrupts (x86)
asm("nop");
```

The string is passed verbatim to the assembler. No inputs, outputs, or clobbers are specified.

### 15.2 Extended Form

The extended form follows the layout and constraints of GCC inline assembly. Constraint strings are LLVM constraint codes, which share GCC's common letters; on x86 targets (x86-64 and 32-bit x86) the GCC single-register letters are translated to LLVM's explicit form, as clang does, and every other constraint is passed to LLVM as written:

```
asm("template" : outputs : inputs : clobbers);
```

```eskiu
asm("outb ${0:b}, $1" :: "a"(val), "Nd"(port) : "memory");   // x86
asm("add $0, $1, $2" : "=r"(sum) : "r"(a), "r"(b));     // AArch64
asm("addq $1, $0" : "+r"(acc) : "r"(b));                // x86-64
```

- **Template**: the assembly instruction string; operands are referenced LLVM-style by `$0`, `$1`, … (with modifiers like `${0:b}` for a sub-register), not `%0`/`%1`. The outputs are numbered first, then the inputs, as in GCC and clang.
- **Outputs**: list of `"constraint"(lvalue)` pairs, written by the asm. A constraint starts with `=` (written only: `"=r"`, the early-clobber `"=&r"`, a specific register such as `"=a"` or `"={ax}"`, or memory `"=m"`) or `+` (read and written: `"+r"`, `"+m"`). The operand must be a writable lvalue (a variable, field, element or dereference, not a bitfield or a `const`) of an integer type other than `bool`, a floating-point type or a pointer. A register output is a result of the asm stored into the lvalue afterwards; a memory output passes the lvalue's address. A `+r` output also feeds the lvalue's current value in through an input tied to it.
- **Inputs**: list of `"constraint"(expr)` pairs (a constraint may not start with `=` or `+`). Common constraints: `"r"` (any register), `"m"` (memory), `"i"` (an immediate), and a specific register. On x86 the GCC letters name one register each: `"a"`, `"b"`, `"c"`, `"d"`, `"S"` and `"D"` become `{ax}`, `{bx}`, `{cx}`, `{dx}`, `{si}` and `{di}` (al/ax/eax/rax by operand width), keeping the modifiers (`"=a"`, `"+a"`, `"=&d"`) and the other letters of a combined constraint (`"Nd"`, an 8-bit immediate or dx, is `"N{dx}"`). LLVM's explicit form, a register in braces such as `"{ax}"` or `"N{dx}"`, is accepted as written. On other targets every constraint is passed to LLVM unchanged.
- **Clobbers**: comma-separated list of clobbered resources. `"memory"` tells the compiler that the asm may read or write arbitrary memory (acts as a compiler barrier).

Sections are separated by `:`. Trailing sections may be omitted if empty.

### 15.3 Notes

- Inline assembly is only meaningful when targeting a platform whose assembler understands the instructions. Use `--target` to select the appropriate triple (see §16).
- `asm` is a statement, not an expression. It does not produce a value; results come back through output operands.
- Beyond the output rules above, the compiler performs no validation of the assembly template or constraints; they are forwarded to LLVM.

---

## 16. CLI Flags

| Flag | Action |
|------|--------|
| `eskiuc --version` | Print the compiler and LLVM version |
| `eskiuc file.esk -o prog` | Compile **and link** into the executable `prog` |
| `eskiuc a.esk b.esk -o prog` | Compile several files together (declarations are merged) |
| `eskiuc run file.esk [args...]` | Compile to a temp executable, run it (forwarding `args`), delete it; exit code propagated |
| `eskiuc run --asan file.esk` | Same, with a compiler flag (flags precede the script, program args follow) |
| `eskiuc repl [file.esk …]` | Interactive session: each input is JIT-compiled and run; the files are loaded first |
| `eskiuc fmt file.esk …` | Reformat files in place (one statement per line, spacing, indentation; preserves tokens and comments) |
| `eskiuc fmt --check file.esk …` | Report files that are not formatted (exit non-zero); write nothing |
| `eskiuc file.esk --asan -o prog` | Instrument with AddressSanitizer (memory errors) and link its runtime |
| `eskiuc file.esk --ubsan -o prog` | Insert trapping bounds checks (traps on out-of-bounds; no runtime) |
| `eskiuc file.esk --safe -o prog` | Run-time safety checks that trap instead of continuing: every array and slice index is bounds-checked, a signed `+`, `-` or `*` on `int`/`int64` that overflows traps (instead of wrapping), and dereferencing a null pointer (`*p`, `p.f`, `p[i]`) traps; off by default |
| `eskiuc file.esk --safe=bounds,null -o prog` | Only the named checks, a comma-separated subset of `bounds` (array and slice indexes, slice ranges), `overflow` (signed `+ - *` on `int`/`int64`) and `null` (dereferencing a null pointer). An unknown name is an error that lists the valid ones. A bare `--safe` is all three |
| `eskiuc file.esk -Wall -o prog` | Enable lint warnings in the program's own files (not in imported modules): unused vars/params/functions, assignment-in-condition, a local that may be used uninitialized |
| `eskiuc file.esk -Wextra -o prog` | Extra warnings on top of `-Wall`: signed/unsigned comparison mismatches |
| `eskiuc file.esk -Wno-format -o prog` | Turn off one of the default-on warnings (see Diagnostics): `-Wno-string-compare`, `-Wno-format`, `-Wno-address-of-packed-member`, `-Wno-dangling-pointer`, `-Wno-frame-larger-than`, `-Wno-unused-result`, `-Wno-await-holding-lock`, `-Wno-deprecated` |
| `eskiuc file.esk -Werror -o prog` | Report every warning (default-on, `-Wall`, `-Wextra`, deprecated) as an error: the build fails |
| `eskiuc file.esk -w -o prog` | Print no warnings at all |
| `eskiuc file.esk -O2 -o prog` | Optimize: run the LLVM middle-end (`-O1`/`-O2`/`-O3`). `-O0` (default) emits naive IR straight to the backend. A level above 3 is rejected |
| `eskiuc file.esk -o prog -lfoo` | Link, passing library flags through to the linker |
| `eskiuc file.esk -o prog --link-arg=ARG` | Pass an extra argument to the linker (repeatable) |
| `eskiuc file.esk -o prog --no-default-libs` | Link only what `-l` names: skip `#pragma link` and the implied runtimes |
| `eskiuc file.esk -o file.o` | Compile to an object file only (no link) |
| `eskiuc file.esk -o libfile.a` | Compile to a static library (`.a`, or `.lib`) |
| `eskiuc file.esk -o libfile.so` | Link a shared library (`.so`, `.dylib`, `.dll`) |
| `eskiuc file.esk -o prog --subsystem=windows` | Link a Windows program for the GUI subsystem (no console) |
| `eskiuc file.esk -c -o name` | Compile to an object file only, any name |
| `eskiuc file.esk` | Compile to `file.esk.o` (object only) |
| `eskiuc file.esk --target TRIPLE` | Cross-compile for the given target triple |
| `eskiuc file.esk --mcpu=CPU --mattr=FEATURES --reloc=MODEL` | Target CPU, feature string (LLVM `-mattr` syntax) and relocation model (`pic`, the default, `static` or `dynamic-no-pic`) |
| `eskiuc file.esk --freestanding` | Object only; use `esk_alloc`/`esk_free` instead of `malloc`/`free` |
| `eskiuc file.esk --test-lexer` | Dump token stream |
| `eskiuc file.esk --test-parser` | Dump AST |
| `eskiuc file.esk --test-typechecker` | Type check only; print errors |
| `eskiuc file.esk --test-codegen` | Dump LLVM IR |
| `eskiuc file.esk --hover-at LINE:COL` | Print inferred type at position |
| `eskiuc file.esk --definition-at LINE:COL` | Print definition location of symbol |
| `eskiuc file.esk --complete-at LINE:COL` | Print completions at position, one `kind<TAB>name<TAB>detail` per line |
| `eskiuc file.esk --symbols` | Print the file's declarations (an outline) |

**Linking.** When the `-o` output is not an object file (no `.o` suffix) and `-c`
is absent, `eskiuc` links the program into an executable by invoking the system
C toolchain (`$CC`, then `cc`/`clang`/`gcc` on the `PATH`) exactly as `rustc`
and `clang` do internally. Without `$CC`, a WebAssembly (emscripten), Android or iOS
`--target` links with that platform's driver instead: `emcc`, the Android NDK's clang, or
`xcrun -sdk iphoneos clang`. An output ending in `.a` or `.lib` is a static library (the
object in an archive), and one ending in `.so`, `.dylib` or `.dll` a shared library
(linked with `-shared`). `--subsystem=windows` links a Windows program for the GUI
subsystem, still entered at `main`. `$CC` may include arguments (`CC="clang --target=..."`). `-l<lib>` and `-L<path>` flags, and any `--link-arg=<arg>`,
are forwarded to the linker. A C toolchain must therefore be installed (it is the
only build-time dependency besides LLVM).

The driver also adds the libraries the program itself implies, so the usual ones
never need a flag. It links each `#pragma link("name")` library the program and
its imports name (the stdlib uses this for `libm` on Linux, `pthread` on Linux and
Windows, and `ws2_32` on Windows). It links the C++ exception runtime when the
program throws or catches (`-lc++` on macOS, `-lstdc++` on Linux and Windows) and
`-lpthread` when it calls `thread_create` (Linux and Windows). These come after
every object and `--link-arg`, once each, and a library already given with `-l`
is not repeated. `--no-default-libs` turns all of them off for custom linking.
Nothing is added when no executable is linked. With `--freestanding` (or a `.o` output)
no linking happens, so bare-metal targets are linked yourself (see the `ld.lld`
invocation in `kernel/Makefile`).

**Running directly.** `eskiuc run file.esk [args...]` compiles to a temporary executable, runs it (forwarding `args...`), then deletes it, propagating the program's exit code, handy for quick iteration. If the program is killed by a signal, `run` exits with `128 +` the signal number, as a shell does. Compiler flags (including ones that take a value, such as `--target TRIPLE`) go *before* the script and program arguments *after* it; a `--` right after the script is dropped, so `eskiuc run --asan file.esk -- input.txt` passes only `input.txt`. Because a leading `#!` line is ignored, a script can also start with `#!/usr/bin/env -S eskiuc run` and, once `chmod +x`'d, be executed directly.

**REPL.** `eskiuc repl` reads inputs from standard input and runs each one through the LLVM ORC JIT in the compiler's process (prompts `> ` and `... ` are shown only when standard input is a terminal). An input is a group of top-level declarations (functions, structs, enums, unions, interfaces, type aliases, `import`s) or a sequence of statements; it continues on the next line while it is unfinished: a bracket, a string literal, a block comment or an `#if`/`#ifdef` is still open; the line ends with an operator (`1 +`, `x > 3 ?`) or with a header whose body comes next (`int sq(int n)`, `for (i in 0..3)`, `struct P`, `else`, `= int(int v)`); or the input parses neither as declarations nor as statements only because it ends early (a `do { ... }` before its `while`). An `if` or a `try` waits one more line for an `else`, `catch` or `finally` (any other line runs it first). A blank line sends a pending input as it is, unless a bracket is open. A line that starts with an operator (`? 10`, `.len`, `&& b`) is reported, since the line before has already run: break a long expression after the operator instead. Statements run as the body of a fresh function. A variable declared at their top level (`int x = f();`, or `let x = expr;`, where the REPL infers the type of `expr`) becomes a session variable, a global assigned when the input runs; declaring it again with the same type assigns the same variable. When the last statement is an expression other than an assignment, an increment or a call to a variadic C function, its value is printed by type: numbers, `bool`, `char` and `string` as literals, pointers as addresses, structs field by field (`Point{x: 1, y: 2}`), enums and sum types by member (`Rect(2, 3)`), arrays, slices and `List<T>` as `[a, b]`, `String` as its text, other types as `<Type>`. Every input is type-checked together with the declarations accepted so far, and a later declaration of a name replaces the earlier one for later inputs; code compiled before keeps the function or variable it was linked against (redefining a type recompiles every function and resets the session variables of that type to zero). An input that fails to parse or type-check prints its diagnostics and leaves the session unchanged; an exception that leaves an input is reported (`error: uncaught exception: int 5`) and the session continues. A crash in an input (a bad pointer, a trap) is reported too on Unix, but the process may be left in a bad state. The REPL is not available on Windows in this release (the JIT does not register the unwind tables Windows exceptions need): `eskiuc repl` there says so and exits 1; run files with `eskiuc run`. An input that contains `await` runs as the body of an `async` function, and the REPL drives its future on the session's event loop (`repl_loop()`, an `EventLoop*` created on first use, with `<future>` and `<eventloop>` imported into the session) until it completes; when nothing left on the loop could complete it (no pending timer and no watched descriptor), the input reports `error: the awaited future did not complete`. Commands: `:type EXPR` (print the type, run nothing), `:load FILE` (add the file's declarations), `:reset`, `:help`, `:quit` (or end of input). The self-hosted `eskiuc-esk` has no REPL.

**Formatting.** `eskiuc fmt file.esk …` reformats files in place. It changes only the blanks between tokens and the line breaks, so it never alters a program's tokens or behavior (only `__LINE__` and diagnostics positions move), and it is idempotent:

- One statement per line: a line is split after each statement's `;` and around block braces, so `if (a) { return 1; }` takes three lines and `x = 1; y = 2;` two. `} else {`, `} while (c);` and `});` stay together; an empty block stays `{}`; initializer braces (`{1, 2}`, `P{x: 1}`) and enum bodies stay on their line. A trailing `//` comment stays with the statement's first line.
- Spacing: one blank between tokens, one around assignment, comparison and logical operators (`=`, `+=`, `==`, `<=`, `&&`, ...), after `,` and `;`, after `if`/`while`/`for`/`switch`/`catch`/`match` and before a block `{`; none before `,` `;` `)` `]` or after `(` `[`. Arithmetic operators keep the spacing they were written with.
- Type argument lists: no blank around `<` or before `>`, one after each comma (`List < int >` and `HashMap<string,int>` become `List<int>` and `HashMap<string, int>`). The formatter cannot tell a type from a comparison, so it rewrites a `<...>` only when it can only be a type argument list: the `<` follows a name that is not a keyword, with no blank between unless the name is capitalized (`a < b > c` and `f(a < b, c > (d))` are comparisons); everything up to the matching `>` on the same line is type syntax (names, `*`, `?`, commas, `[N]` array suffixes, `fn(...) -> R` types, nested lists); and the token after the `>` is a name, `(`, `{`, `)`, `,`, `;`, `[` or `*`. Any other `<` and `>` keep the spacing they were written with. Two `>` written apart (`List<List<int> >`) keep their blank, so they never become `>>`.
- Indentation: four spaces per brace level. A line that continues a statement (inside open parentheses, or after a line that does not end one) keeps its indentation relative to the statement's first line, at least four more.
- A blank line after each top-level definition; other blank lines are kept, those at the end of the file dropped, and the file ends with one newline. Trailing whitespace is trimmed, except after a trailing `\`.
- Comments, every byte of a string or character literal (every line of a string that spans lines too), lines continued with `\` and preprocessor lines (at column 0) keep their bytes.
- Long lines are not broken: a line only gets shorter when it holds several statements.

`--check` makes it report (and exit non-zero on) files that would change, without writing. Useful in CI.

**Sanitizers.** `--asan` instruments the program with AddressSanitizer (detecting heap, stack and global memory errors) and links the matching LLVM runtime; `--ubsan` inserts trapping bounds checks (an out-of-bounds access aborts via a trap; no runtime is required). Both are real LLVM instrumentation passes and compose with `eskiuc run`.

`--hover-at`, `--definition-at` and `--complete-at` accept the format `LINE:COL` with 1-based line and column numbers. They are used by the VS Code extension to provide hover type information, go-to-definition navigation and completion. The self-hosted `eskiuc-esk` implements these modes and the `-Wall`/`-Wextra` warnings with the same output.

### Cross-compilation

`--target TRIPLE` sets the LLVM target triple for the output object file. The AArch64, X86, 32-bit ARM and WebAssembly LLVM backends are included in the Eskiu build. On WebAssembly, exceptions work through emscripten's runtime (`wasm32-unknown-emscripten`); on the other wasm32 triples `try` and `throw` are errors. `--mcpu`, `--mattr` and `--reloc` select the CPU, the feature string and the relocation model. See [cross-compile.md](../dev/cross-compile.md) for worked examples.

```bash
eskiuc kernel.esk --target x86_64-pc-linux-gnu --freestanding -o kernel.o
eskiuc kernel.esk --target aarch64-unknown-none --freestanding -o kernel.o
```

Common triples:

| Triple | Description |
|--------|-------------|
| `x86_64-pc-linux-gnu` | ELF x86-64 (Linux) |
| `aarch64-unknown-linux-gnu` | ELF AArch64 (Linux) |
| `aarch64-unknown-none` | Bare-metal AArch64 |
| `x86_64-unknown-none` | Bare-metal x86-64 |
| `x86_64-pc-windows-gnu` | COFF x86-64 (Windows, MinGW) |
| `i686-pc-linux-gnu` | ELF 32-bit x86 (Linux) |
| `armv6k-none-eabihf` | Bare-metal 32-bit ARM, hard float (e.g. the Nintendo 3DS with `--mcpu=mpcore`) |
| `wasm32-unknown-emscripten` | WebAssembly through emscripten (browsers and node) |
| `wasm32-wasip1` | WebAssembly with WASI |
| `aarch64-linux-android24` | Android, 64-bit ARM, API level 24 (also `armv7a-linux-androideabi24`, `x86_64-linux-android24`) |
| `arm64-apple-ios15.0` | iOS devices (`arm64-apple-ios15.0-simulator` for the simulator) |

When `--target` is omitted the compiler defaults to the host machine's triple.

---

## 17. Error Reporting

The compiler emits diagnostics with full source location information:

```
error: file.esk:8:22: undefined variable 'foo'
error: file.esk:14:5: initializing 'p': cannot convert 'float' to 'int*'
```

The format is `error: file:line:col: message`. Line and column numbers are 1-based. Lexer, preprocessor, parser and type errors all use it, and an error inside an imported file names that file. `-Wall` warnings are printed as `file:line:col: warning: message`.

A few warnings are on by default, because the code they flag is almost always a mistake. Each ends with its flag and is turned off with the matching `-Wno-` option; none of them rejects a program, and code from an imported module is not checked:

| Warning | Flags |
|---|---|
| `-Wstring-compare` | `==` / `!=` with a string literal operand: it compares two addresses, so equal text in two places compares unequal. Use `strcmp(a, b) == 0` or `String_eq_cstr` |
| `-Wformat` | A literal format string of `printf`, `fprintf`, `dprintf`, `sprintf`, `snprintf`, `String_format`, `String_appendf` or a `log_*` function whose conversions do not match the arguments after C's default promotions (`%d` with an `int64`, `%s` with an `int` or a `String`, `%f` with an integer), a missing or extra argument, an unknown conversion. `%l` is 64 bits except on Windows targets, where `long` is 32 |
| `-Waddress-of-packed-member` | `&s.f` (or `&s.a[i]`) where the struct's packing aligns `f` below its type's alignment: the pointer may be unaligned, and strict-alignment CPUs (32-bit ARM) fault on a load through it |
| `-Wdangling-pointer` | Returning a local pointer variable whose every assignment so far stored the address of a local (`*int p = &x; return p;`), or a struct literal holding a local's address (`return H{&y};`). With `-Wall`, also storing a local's address in a global outside `main` |
| `-Wframe-larger-than` | A local larger than 1 MiB: a thread's stack is 512 KiB (secondary threads on macOS) to 8 MiB, so allocate it with `alloc<T>` or make it `static` |
| `-Wunused-result` | A call used as a statement (`f(x);`, `x.m();`, `f<T>(x);`) whose value is a `Result<...>` (the `<result>` type, also through a type alias): the error it may carry is dropped. Use the value, propagate it with `?`, or discard it on purpose with `(void)f(x);` |
| `-Wawait-holding-lock` | In an `async` function, an `await` while a `<threading>` `Mutex` is locked: the task parks holding the lock, and another task on the same thread that locks it deadlocks. The rule is per block: a statement that is a lock call (`Mutex_lock(&m)`, `Mutex_lock(p)`, or `m.lock()` on a `Mutex` / `*Mutex`) on a place spelled with identifiers and fields (`m`, `self.m`; `&` is dropped), followed in the same block by a statement that contains an `await` before a statement that is the unlock call on the same place. A `defer` statement does not unlock (its body runs at the end of the block) and its body is not searched. The warning is at the earliest `await` of that statement, once per lock. An unlock inside a nested statement does not end the region, and other lock shapes are not tracked |

Every mode, including the `--test-*` modes, exits with a non-zero status when it reports an error.

Reading a scalar local (a number, `bool`, `char`, pointer, `string` or fn value) declared without an initializer before any assignment to it is an error ("use of uninitialized variable"), although such a local holds zero (11.1): a read nothing assigns is usually a mistake. It applies when the read is in the function's straight-line prefix, where no path assigns it. Elsewhere a read that some path reaches without an assignment is not an error, since the program's logic may guarantee it (as in C), but `-Wall` reports it as `variable 'x' may be used uninitialized`. The check follows every path through `if`/`else`, loops (a loop body may run zero times; the loop exits when its condition fails or at a `break`), `switch` fall-through, `match` arms, `try`/`catch`/`finally` (a handler or the `finally` may run before the body assigned anything), `defer` bodies (at each exit that runs them), early `return`/`break`/`continue`, and a lambda, which reads the variables it captures when it is created. Taking the address (`&x`) counts as an assignment, and `sizeof(x)` is not a read. Only whole-variable reads of scalar locals are tracked: a struct, union, array, slice or interface local set field by field or element by element never warns. `static` locals start at zero and are never reported, and the body of an `async` function is not checked.

---

## 18. Preprocessor

A small text pass runs before lexing. It supports **object-like and function-like macros** and **conditional compilation**. Directives occupy their own line (the first non-blank character is `#`); a line inside a string literal that spans lines is string text, never a directive, and no macro expands in it. Both directive lines and skipped lines are blanked out so reported line numbers match the original source.

| Directive | Effect |
|---|---|
| `#define NAME value` | Object-like macro; later occurrences of `NAME` are replaced by `value` |
| `#define NAME(a, b) body` | Function-like macro; `NAME(x, y)` substitutes the arguments into `body` |
| `#define NAME` | Define `NAME` with an empty value (useful for `#ifdef`) |
| `#undef NAME` | Remove a definition |
| `#ifdef NAME` / `#ifndef NAME` | Begin a block compiled only if `NAME` is / is not defined |
| `#if expr` / `#elif expr` | Begin a block / an alternative compiled only if the integer expression is non-zero |
| `#else` / `#endif` | Else branch / end of a conditional |
| `#error message` | Abort compilation with `message` (only on an active branch) |
| `#pragma pack(...)` | Struct packing directive; see §8.9 |
| `#pragma link("name")` | Link the executable with `-lname` (see below) |

An `#if`/`#elif` expression is a C-style integer constant expression: integer and
character literals, macros (expanded first), `defined NAME` / `defined(NAME)`, the
unary `! ~ - +`, the arithmetic, shift, relational, equality, bitwise and logical
operators, `?:` and parentheses. An identifier left after expansion counts as `0`.
It is evaluated on 64-bit signed integers with two's-complement wraparound (so
`INT64_MIN / -1` is `INT64_MIN`), and a character literal decodes with the same
escapes as in code. `&&`, `||` and `?:` do not evaluate the operand they skip, so
`#if 1 || 1 / 0` is fine. A division by zero that is evaluated, a shift count
outside 0..63, and an integer literal that does not fit in 64 bits are errors.

Any other directive is an error, as is `#include` (use `import`), an `#else`,
`#elif` or `#endif` without its `#if`, a second `#else`, and a conditional left
open at the end of the file. Inside a skipped branch, unknown directives are
ignored. Macro bodies may not use `#` (stringification) or `##` (token pasting).
Arguments to a function-like macro are split at top-level commas (string and
character literals stay whole) and are macro-expanded before substitution, so
`SQ(SQ(2))` works as in C. An invocation must pass as many arguments as the macro
has parameters (a one-parameter macro accepts `F()` as one empty argument), and an
argument list left open at the end of the line is an error. A replacement that ends
in the name of a function-like macro picks up the `(...)` that follows it, so after
`#define CALLF F`, `CALLF(2)` expands `F(2)`, also when the `(` is on a later line. Comments count as whitespace: a `//`
or `/* */` comment in a directive is not part of the macro body, and one inside an
argument list does not end the argument. Files with CRLF line endings are handled,
including `\` continuations.

Two predefined macros expand in place: `__LINE__` (the current source line, an
integer) and `__FILE__` (the current file path, a string literal). Together with
`#error` they support assertions and build guards:

```eskiu
#ifndef CONFIG_OK
#error "build CONFIG_OK is required"
#endif
printf("%s:%d: reached\n", __FILE__, __LINE__);
```

A line ending in a backslash (`\`) is **continued** onto the next line, so a macro body may span several physical lines (the spliced lines stay counted, so line numbers are preserved):

```eskiu
#define MAX 100
#define SQ(x) ((x) * (x))
#define DEBUG

#define POLY(x)        \
    ((x) * (x)         \
     + 2 * (x) + 1)

import <io>;

int main() {
    int n = MAX;          // 100
    int s = SQ(n);        // ((100) * (100))
    int p = POLY(3);      // ((3) * (3) + 2 * (3) + 1) = 16
#ifdef DEBUG
    printf("debug build\n");
#endif
    return 0;
}
```

Substitution is identifier-aware and leaves string and character literals untouched. Expansion is **recursive**: a macro whose body references other macros is expanded fully (a macro is never re-expanded within its own expansion). The macro table is **shared across files** and follows the text in order, like C's `#include`: an imported file sees the macros defined before its `import` line (not the ones defined after it), its own `#define`s reach the importing file's later lines, and each input of a multi-file compile sees the macros of the inputs before it. A function-like macro invocation may span lines: its argument list continues until the matching `)` (a newline inside it reads as a space), and the `(` itself may start a later line, past blank lines and comments (`ADD` then `(1, 2)` on the next line is a call, as in C). A function-like macro name with no `(` after it is left as a plain name.

Unlike the other directives, `#pragma` is not consumed by the preprocessor. It is passed through to the compiler. `#pragma pack` (§8.9) and `#pragma link` are acted upon; any other pragma is ignored. A pragma may appear at the top level, inside a function body or inside a struct body; a `pack` inside a struct body applies to the structs declared after it (as in C), not to the enclosing one.

`#pragma link("name")` asks the driver to link the executable with `-lname`. The
name is what follows `-l` (letters, digits, `_ . + -`, not starting with `-`), in
double quotes; any other form is an error. A pragma in an imported module counts
like one in the main file, each library is linked once however many files name
it, and the order of first appearance is kept. Because the preprocessor has
already run, a pragma inside an inactive `#ifdef` branch has no effect, which is
how a module links a library on one platform only:

```eskiu
#ifdef __linux__
#pragma link("m")      // glibc keeps libm apart from libc
#endif
```

The libraries are only used when `eskiuc` links an executable; `-c`, a `.o`
output and `--freestanding` ignore them, and `--no-default-libs` drops them.

### Predefined macros

The compiler predefines these:

| Macro | Value | Notes |
|---|---|---|
| `__LINE__` | current source line, an integer | refreshed for every logical line; on a `\`-continued line it is the line where that logical line starts |
| `__FILE__` | current file path, a string literal | the path as passed to the compiler or resolved by `import`, with `\` and `"` escaped; distinct per file in a multi-file build |
| `__APPLE__` | `1` (Apple targets) | target-OS macro, from `--target` or the host |
| `__linux__` | `1` (Linux targets) | |
| `_WIN32` | `1` (Windows targets) | also defined for 64-bit Windows |
| `_WIN64` | `1` (64-bit Windows targets) | |
| `__aarch64__` / `__x86_64__` / `__arm__` | `1` | target-architecture macro, from `--target` or else the build host (at most one is defined) |
| `__ESKIU_FREESTANDING__` | `1` under `--freestanding` | lets stdlib target `esk_alloc`/`esk_free` instead of libc |

At most one OS family is defined. A bare-metal triple (OS `none`, e.g. `aarch64-none-elf`) defines none of them.

`__LINE__` and `__FILE__` are ordinary object-like macros (so substitution
respects identifier boundaries and skips string/char literals) but their values
are maintained by the compiler. `__LINE__` is re-set to the physical line number
of each logical line before that line is expanded. Line splicing happens first, so
a `__LINE__` on the second physical line of a `\`-continued line reports the line
where the logical line starts, not the line where it textually appears.
`__FILE__` is threaded from the file currently being compiled or imported, so in
a multi-file or `import`-driven build each file sees its own path. Together with
`#error` they support assertions, build guards, and `file:line` diagnostics:

```eskiu
printf("%s:%d: reached\n", __FILE__, __LINE__);   // e.g. "src/main.esk:42: reached"
```

The host-OS macros let stdlib and user code branch on platform with `#ifdef`.
This is how `<net>` selects the correct `sockaddr_in` layout:

```eskiu
#ifdef __APPLE__
    // macOS-specific layout / constants
#else
    // Linux
#endif
```

### Shebang lines

If the first line of a file begins with `#!` (e.g. `#!/usr/bin/env -S eskiuc run`),
the preprocessor ignores it and blanks it out, preserving line numbers. This lets a `.esk` file be marked executable
(`chmod +x`) and run directly as a script; see `eskiuc run` in §16. The `-S` makes `env` split
`eskiuc run` into two arguments: Linux passes everything after the interpreter path as one
argument, so `#!/usr/bin/env eskiuc run` looks for a program named `eskiuc run` there (macOS
splits it either way). `env -S` is in GNU coreutils 8.30 and later and in macOS.
