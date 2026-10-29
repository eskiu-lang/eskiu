# Getting Started with Eskiu

A hands-on introduction to the Eskiu language. You will go from zero to writing
and inspecting real compiled programs in about 30 minutes.

All code blocks in this document compile and run with **Eskiu v0.14.0**.

---

## Installation

Prebuilt releases for macOS and Linux install with one command; see the
[README](../../README.md#install). To build from source, follow the steps below
(more detail, Windows and troubleshooting are in [build.md](build.md)).

### Prerequisites

| Tool         | Minimum version | Notes                             |
| ------------ | --------------- | --------------------------------- |
| LLVM         | 21+             | Headers and libraries required (CI uses 22) |
| CMake        | 3.20+           | Build system                      |
| C++ compiler | C++17           | clang++ or g++                    |
| C compiler   | any recent      | `cc`, `clang` or `gcc`, used to link executables |

### macOS

```bash
brew install llvm cmake
export LLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm
```

Add the `export` line to `~/.zshrc` to make it permanent. Then clone and build:

```bash
git clone https://github.com/eskiu-lang/eskiu.git
cd eskiu
cmake -S . -B build
cmake --build build -j$(sysctl -n hw.ncpu)
```

### Linux (Ubuntu / Debian)

```bash
sudo apt-get install -y git cmake llvm-22-dev clang-22 build-essential
git clone https://github.com/eskiu-lang/eskiu.git
cd eskiu
cmake -S . -B build -DLLVM_DIR=/usr/lib/llvm-22/lib/cmake/llvm
cmake --build build -j$(nproc)
```

### Verify

```bash
./build/eskiuc --version
# Eskiu 0.14.0 (LLVM 22.1.6)   (exact LLVM version depends on your install)
```

Add `./build` to your `PATH` so you can type `eskiuc` from any directory.

---

## Your First Program

Create `hello.esk`:

```eskiu
import <io>;

int add(int a, int b) {
    return a + b;
}

int main() {
    int result = add(5, 3);
    printf("Hello from Eskiu!\n");
    printf("Result: %d\n", result);
    return 0;
}
```

Compile and run. When the `-o` name has no `.o` extension, `eskiuc` links the
program into an executable for you (it calls your system C toolchain under the
hood, exactly as `rustc` does):

```bash
eskiuc hello.esk -o hello
./hello
```

Prefer the two steps, for example to inspect or reuse the object file? Give the
output a `.o` name (or pass `-c`) and link it yourself:

```bash
eskiuc hello.esk -o hello.o   # object only
clang hello.o -o hello        # link
./hello
```

Expected output:

```
Hello from Eskiu!
Result: 8
```

### Run it like a script

You don't have to compile and link by hand while iterating. `eskiuc run` compiles to a
temporary executable, runs it, and cleans up, forwarding any arguments after the script:

```bash
eskiuc run hello.esk
```

Add a shebang line and a `.esk` file becomes directly executable, the way a Python or Ruby
script is:

```eskiu
#!/usr/bin/env -S eskiuc run
import <io>;
int main() { printf("hello\n"); return 0; }
```

```bash
chmod +x hello.esk
./hello.esk
```

For an optimized build, pass `-O2` (or `-O1`/`-O3`), which runs the LLVM middle-end before
code generation. The default is `-O0`, which emits naive IR straight to the backend.

### Try it in the REPL

`eskiuc repl` starts an interactive session. Each input is compiled with LLVM's JIT and
run at once. A bare expression prints its value, and declarations (functions, structs,
enums, imports) stay available to later inputs:

```
$ eskiuc repl
> 1 + 2
3
> int sq(int n) { return n * n; }
> let x = sq(7);
> x + 1
50
> struct Point { int x; int y; }
> Point{3, 4}
Point{x: 3, y: 4}
> import <list>;
> let xs = List<int>{};
> xs.push(5);
> xs
[5]
```

`let x = expr;` at the prompt declares a session variable, and its type comes from the
expression. Declaring a function or variable again replaces it for later inputs. An input
that does not parse or type-check prints the error and changes nothing; an uncaught
exception is reported and the session goes on. Input continues on the next line (prompt
`...`) while it is unfinished, in any brace style: an open bracket or string, a line that
ends with an operator, or a header such as `int sq(int n)` whose `{` comes next; an `if`
or `try` block waits one line for its `else` or `catch`, and a blank line sends what is
pending. `:type EXPR` prints a type without running anything,
`:load FILE` adds a file's declarations, `:reset` starts over and `:quit` (or Ctrl-D) leaves.

The prompt also takes `await`. The input runs on an event loop the session keeps, which
`repl_loop()` returns, so a timer or an async read can be registered on it:

```
> import <timer>;
> async int twice(int x) { await timer_after(repl_loop(), 10); return x * 2; }
> await twice(21)
42
> let v = await twice(5);
```

When the awaited future cannot complete (nothing left on the loop could wake it), the REPL
prints `error: the awaited future did not complete` instead of waiting forever.

### Peek at the generated IR

Before producing an object file you can ask the compiler to print the LLVM IR it
would emit:

```bash
eskiuc hello.esk --test-codegen
```

Abridged output:

```llvm
@0 = private unnamed_addr constant [19 x i8] c"Hello from Eskiu!\0A\00", align 1
@1 = private unnamed_addr constant [12 x i8] c"Result: %d\0A\00", align 1

declare i32 @printf(ptr, ...)

define i32 @add(i32 %a, i32 %b) {
entry:
  %b2 = alloca i32, align 4
  %a1 = alloca i32, align 4
  store i32 %a, ptr %a1, align 4
  store i32 %b, ptr %b2, align 4
  %0 = load i32, ptr %a1, align 4
  %1 = load i32, ptr %b2, align 4
  %2 = add i32 %0, %1
  ret i32 %2
}

define i32 @main() {
entry:
  %result = alloca i32, align 4
  %0 = call i32 @add(i32 5, i32 3)
  store i32 %0, ptr %result, align 4
  ...
  ret i32 0
}
```

String literals become private globals. At the default `-O0`, parameters and local
variables are stack slots (`alloca`). Add `-O2` to see the optimized IR, where `add`
is a single `add i32` instruction.

---

## Variables and Types

### Declaring variables

Eskiu supports two equivalent declaration styles:

```eskiu
// C-style
int x = 42;
float pi = 3.14;
bool flag = true;

// let-style
let x: int = 42;
let pi: float = 3.14;
let flag: bool = true;
```

Both styles compile to the same IR. Use whichever reads more clearly in context.

A variable declared without a value starts at zero: `let o: Opts;` has every field zero
(null pointers and strings, empty arrays), and so does `int[16] buf;`. This differs from C,
where such a variable holds whatever was in memory. Reading a number or pointer you never
assigned is still a compile error, since it is usually a mistake.

### Primitive types

| Type     | Width    | Notes                           |
| -------- | -------- | ------------------------------- |
| `bool`   | 1 bit    | `true` / `false`                |
| `char`   | 8-bit    | Unsigned; single character      |
| `int`    | 32-bit   | Signed                          |
| `int8`   | 8-bit    | Signed                          |
| `int16`  | 16-bit   | Signed                          |
| `int32`  | 32-bit   | Alias for `int`                 |
| `int64`  | 64-bit   | Signed                          |
| `uint`   | 32-bit   | Unsigned                        |
| `uint8`  | 8-bit    | Unsigned (byte)                 |
| `uint16` | 16-bit   | Unsigned                        |
| `uint32` | 32-bit   | Unsigned                        |
| `uint64` | 64-bit   | Unsigned                        |
| `float`  | 32-bit   | IEEE 754 single-precision (literals are `double`; assigning coerces down) |
| `double` | 64-bit   | IEEE 754 double-precision       |
| `string` | pointer  | Null-terminated C string        |
| `void`   | n/a      | Used as function return type    |

### Integer and hex literals

```eskiu
int a = 255;
int b = 0xFF;    // same value, hex prefix supported
uint8 mask = 0x0F;
int64 big = 1000000;
int neg = -42;   // negative literals work anywhere, including global scope
float f = -3.14;
int perms = 0755;  // a leading 0 means octal, as in C (493)
```

Literals follow C. `08` (an invalid octal digit) and `0x` with no digits are
errors, and so is any suffix: write `1000` rather than `1_000`, and `3.5` rather
than `3.5f`.

### Pointer types

Prefix `*` to make a type a pointer:

```eskiu
let p: *int = null;
int x = 10;
p = &x;           // address-of
int val = *p;     // dereference
```

---

## Operators

### Arithmetic

```eskiu
int a = 10;
int b = 3;
int sum  = a + b;   // 13
int diff = a - b;   // 7
int prod = a * b;   // 30
int quot = a / b;   // 3
int rem  = a % b;   // 1
```

As in C, operands narrower than `int` (`bool`, `char`, `int8`, `uint8`, `int16`,
`uint16`) are promoted to `int` before arithmetic and comparison. So
`(uint8)200 + (uint8)100` is `300`, and a `uint8` compared with a negative `int8`
compares the values, not the bit patterns. Assigning the result back to a narrow
variable truncates it.

### Comparison

All comparison operators work on integers, floats, and pointers:

```eskiu
bool eq  = (a == b);
bool neq = (a != b);
bool lt  = (a <  b);
bool gt  = (a >  b);
bool lte = (a <= b);
bool gte = (a >= b);
```

### Logical

```eskiu
bool x = true;
bool y = false;
bool both = x && y;   // false
bool either = x || y; // true
bool inv = !x;        // false
```

### Bitwise

```eskiu
int flags = 0xFF;
int low   = flags & 0x0F;   // AND  → 15
int hi    = flags | 0x100;  // OR   → 511
int xord  = flags ^ 0x55;   // XOR  → 170
int shl   = 1 << 4;         // shift left  → 16
int shr   = flags >> 4;     // shift right → 15
int inv2  = ~0;              // bitwise NOT → -1
```

### Compound assignment

```eskiu
int x = 10;
x += 5;    // x = 15
x -= 3;    // x = 12
x *= 2;    // x = 24
x /= 4;    // x = 6
x %= 4;    // x = 2
x &= 0x3;  // x = 2
x |= 0x1;  // x = 3
x ^= 0x2;  // x = 1
x <<= 3;   // x = 8
x >>= 1;   // x = 4
```

### Address-of and dereference

```eskiu
int n = 42;
let ptr: *int = &n;   // address-of
int copy = *ptr;      // dereference → 42
```

### Pointer arithmetic

```eskiu
import <mem>;     // alloc<T> / free
import <io>;

int main() {
    let buf: *uint8 = alloc<uint8>(4);
    *buf = 10;
    *(buf + 1) = 20;
    *(buf + 2) = 30;
    printf("%d %d %d\n", *buf, *(buf + 1), *(buf + 2));
    free(buf);
    return 0;
}
```

`ptr + n` advances by `n * sizeof(*ptr)` bytes: typed arithmetic. Adding `1` to a `*uint8` moves one byte; adding `1` to a `*int` moves four bytes. `*void` and `*char` are the exceptions: they always use byte-level stride.

### Cast

```eskiu
float f = 3.99;
int   i = (int)f;      // truncates → 3
uint8 b = (uint8)255;
```

---

## Control Flow

### if / else

```eskiu
import <io>;

int main() {
    int x = 7;
    if (x > 10) {
        printf("big\n");
    } else if (x > 4) {
        printf("medium\n");
    } else {
        printf("small\n");
    }
    return 0;
}
```

Output: `medium`

### while

```eskiu
import <io>;

int main() {
    int i = 1;
    while (i <= 5) {
        printf("%d\n", i);
        i += 1;
    }
    return 0;
}
```

### for with declaration init

The loop variable can be declared directly in the `for` header:

```eskiu
import <io>;

int main() {
    for (int i = 0; i < 5; i += 1) {
        printf("i = %d\n", i);
    }
    return 0;
}
```

### break and continue

```eskiu
import <io>;

int main() {
    for (int i = 0; i < 10; i += 1) {
        if (i == 3) { continue; }
        if (i == 6) { break; }
        printf("%d\n", i);
    }
    return 0;
}
```

Output: `0 1 2 4 5` (one per line).

### switch / case

```eskiu
import <io>;

int main() {
    int day = 3;
    switch (day) {
        case 1:
            printf("Monday\n");
            break;
        case 2:
            printf("Tuesday\n");
            break;
        case 3:
            printf("Wednesday\n");
            break;
        default:
            printf("Other\n");
            break;
    }
    return 0;
}
```

Output: `Wednesday`

---

## Functions

### Defining and calling

Return type comes first, followed by the name and parameter list:

```eskiu
import <io>;

int square(int n) {
    return n * n;
}

bool isEven(int n) {
    return n % 2 == 0;
}

int main() {
    printf("4^2 = %d\n", square(4));
    if (isEven(10)) {
        printf("10 is even\n");
    }
    return 0;
}
```

### extern for C interop

`extern` declares a function that lives in a C library. The compiler emits an
LLVM `declare` and the linker resolves it:

```eskiu
extern int printf(string fmt, ...);
extern int puts(string s);
extern double sqrt(double x);
```

The `...` marks a variadic function, required for `printf`, `scanf`, etc.

The standard library already declares the common C functions, so most programs
import them instead: `<io>` has `printf`, `puts` and the rest of C stdio, `<mem>` the
memory and string functions (`memcpy`, `strlen`, `strcmp`, `strstr`, ...), `<math>`
the libm ones (`sqrt`, `pow`, ...) and `<process>` `process_exit`. Declare an `extern`
yourself for anything else.

`extern` also declares a **global variable** defined in another translation unit.
Drop the parentheses and end with a semicolon; the variable resolves at link time,
so Eskiu can read and write state shared with a C library:

```eskiu
extern int   opterr;       // libc's getopt flag; read and assign it like any variable
extern float g_volume;     // a global defined in your own C code

void quiet_getopt() {
    opterr = 0;
}
```

### Declaration order

Definition order doesn't matter: a function can call another that's defined later in the file, and mutual recursion works out of the box. You can also write a body-less forward declaration if you prefer to state a signature up front:

```eskiu
int is_odd(int n);                  // forward declaration (optional)

int is_even(int n) {
    if (n == 0) { return 1; }
    return is_odd(n - 1);           // defined below, works
}

int is_odd(int n) {
    if (n == 0) { return 0; }
    return is_even(n - 1);
}
```

---

## Structs

### Defining and using

```eskiu
import <io>;

struct Point {
    float x;
    float y;
}

int main() {
    let p: Point = Point { x: 1.5, y: 2.5 };
    printf("x=%f y=%f\n", p.x, p.y);
    p.x = 10.0;
    printf("x=%f\n", p.x);
    return 0;
}
```

Struct literals accept either named fields (`Point { x: 1.0, y: 2.0 }`) or
positional fields (`Point { 1.0, 2.0 }`). Fields you leave out are zero-filled, as
in C. Each field value is type-checked against the field's type, and naming a
field twice or giving too many values is an error.

### Methods with self

Methods are defined inside the struct body and receive a pointer to the instance
as `self`:

```eskiu
import <io>;

struct Rect {
    float w;
    float h;

    float area() {
        return self.w * self.h;
    }

    void print() {
        printf("Rect(%f x %f)\n", self.w, self.h);
    }
}

int main() {
    let r: Rect = Rect { w: 4.0, h: 3.0 };
    r.print();
    printf("area = %f\n", r.area());
    return 0;
}
```

The compiler lowers `r.area()` to `Rect_area(ptr %r)`: the receiver is passed
as the first argument.

A method can also be written as a free function named `Type_method` whose first
parameter is the receiver. To call it on a `const` value, declare the receiver
`const T* self`; a method with a plain `*T self` may change the value, so calling
it on a `const` is an error:

```eskiu
import <io>;

struct P { int x; }
int  P_get(const P* self) { return self.x; }
void P_set(*P self, int v) { self.x = v; }

int main() {
    const P c = P { x: 4 };
    printf("%d\n", c.get());   // fine: P_get takes const P*
    // c.set(9);               // error: cannot call method 'set' on a read-only value
    return 0;
}
```

Eskiu has no namespaces and no `Type::method` or `Type.method` syntax: a function
belongs to a type through its name. `Type_method` is the function's real name and the
symbol C code links against (there is no name mangling), so `nm`, a debugger and a
stack trace show it as written. Call it with dot syntax on a value
(`s.replace_all("-", "+")`) and by its full name when you need the function itself, to
pass it as a value or to call it from C. Editors offer these functions after `x.`
(see `editor/vscode`).

### Fixed-size array fields

A struct field can be a fixed-size array:

```eskiu
struct Packet {
    uint8[858] left;
    uint8[858] right;
    int        len;
}
```

In LLVM IR this becomes `[858 x i8]`, no heap allocation required.

---

## Enums and `match`

A plain `enum` is a set of named integer constants, like C:

```eskiu
enum Color  { Red, Green, Blue }            // 0, 1, 2
enum Status { Ok = 0, Err = 2, Pending }    // 0, 2, 3

let c: Color = Green;            // c == 1
```

When one or more variants carry a **payload**, the enum becomes an algebraic data type, a tagged union. Variants are constructed by name and destructured with `match`:

```eskiu
import <io>;

enum Shape {
    Circle(float),
    Rect(float, float),
    Unit,                       // a payload-free variant
}

float area(Shape s) {
    match s {
        Circle(r)  -> return 3.14 * r * r;
        Rect(w, h) -> return w * h;          // payload fields bind to w, h
        _          -> return 0.0;            // `_` matches the rest
    }
}

int main() {
    Shape a = Circle(2.0);
    printf("%f\n", area(a));
    return 0;
}
```

A `match` must be **exhaustive**: every variant needs an arm, or there must be a `_` default. Algebraic enums may also be generic (`enum Option<T> { None, Some(T) }`) and are monomorphized per instantiation. See spec §8.7 for the full rules.

---

## Templates

### Template structs

Use angle brackets to parameterize a struct over one or more types:

```eskiu
struct Pair<A, B> {
    A first;
    B second;
}
```

Instantiate by supplying concrete types:

```eskiu
import <io>;

struct Pair<A, B> {
    A first;
    B second;
}

int main() {
    let p: Pair<int, float> = Pair<int, float> { first: 7, second: 3.14 };
    printf("first=%d second=%f\n", p.first, p.second);
    return 0;
}
```

The compiler performs lazy monomorphic instantiation: `Pair<int, float>` becomes
`%Pair_int_float` in IR.

### Template functions

```eskiu
import <io>;

T max<T>(T a, T b) {
    if (a > b) { return a; }
    return b;
}

int main() {
    printf("%d\n", max<int>(3, 5));      // explicit type argument
    printf("%f\n", max<float>(1.2, 0.8));
    printf("%d\n", max(7, 2));            // inferred: T = int
    return 0;
}
```

The return type is `T`: the function returns the same type it is instantiated with. The type argument can be explicit (`max<int>(3, 5)`) or inferred from the arguments (`max(7, 2)`) when the type parameter appears directly as a parameter type.

### Result<T, E> from stdlib

`<result>` provides an error-as-value type ready to use:

```eskiu
import <result>;

import <io>;

Result<int, string> divide(int a, int b) {
    if (b == 0) {
        return Err<int, string>("division by zero");
    }
    return Ok<int, string>(a / b);
}

int main() {
    let r: Result<int, string> = divide(10, 2);
    if (r.ok == 1) {
        printf("result = %d\n", r.value);
    } else {
        printf("error: %s\n", r.error);
    }
    return 0;
}
```

### Bounded generics (constraints)

A type parameter can require that its concrete type satisfy one or more interfaces, written after a colon. The constraint is checked at the instantiation site, not deep in codegen:

```eskiu
interface Ord {
    int cmp(*Ord other);    // `Ord` stands for the implementing type
}

T max<T: Ord>(T a, T b) {
    if (a.cmp(&b) > 0) { return a; }
    return b;
}
```

Inside an interface, a type spelled with the interface's own name stands for the implementing type, so a `struct Num` with `int cmp(*Num other)` satisfies `Ord`. Use `+` to require several interfaces at once (`<K: Hashable + Eq, V>`). A struct satisfies a constraint by defining the interface's methods. A primitive type has no methods, so it satisfies a constraint through a **free function** named like the interface method whose first parameter is that primitive, e.g. `int cmp(int a, *int b)` makes `int` satisfy the `Ord` above (the interface's `*Ord` becomes `*int`), so `max(3, 9)` works. See spec §10.6 for the full rules.

---

## Interfaces

Interfaces declare a set of method signatures. Any struct that provides matching
methods satisfies the interface. No `implements` keyword is needed.

### Declaring an interface

```eskiu
interface Drawable {
    void draw();
}
```

### Implementing structurally

```eskiu
import <io>;

interface Drawable {
    void draw();
}

struct Circle {
    float radius;

    void draw() {
        printf("Circle(r=%f)\n", self.radius);
    }
}

struct Square {
    float side;

    void draw() {
        printf("Square(s=%f)\n", self.side);
    }
}
```

`Circle` and `Square` both satisfy `Drawable` automatically because they
implement a `draw()` method with a matching signature. The whole signature has to
match: a `draw` that returns a different type or takes different parameters does
not satisfy the interface.

### Passing a struct as an interface

Pass a pointer to the struct where an interface parameter is expected. The
compiler auto-boxes it into a fat pointer `{data_ptr, vtable_ptr}` and performs
the vtable dispatch at the call site:

```eskiu
void render(Drawable d) {
    d.draw();
}

int main() {
    let c: Circle = Circle { radius: 5.0 };
    let s: Square = Square { side: 3.0 };
    render(&c);
    render(&s);
    return 0;
}
```

Output:

```
Circle(r=5.000000)
Square(s=3.000000)
```

Pass `&c`, not `c`. An interface value refers to the struct through a pointer, so
passing a struct by value where an interface is expected is a compile error.

An interface value is an ordinary value: it can be a local, a struct field, a
parameter or a return value, and it keeps referring to the struct it was made
from.

```eskiu
let d: Drawable = &c;
d.draw();
d = &s;          // now refers to s
d.draw();
```

---

## Memory

### Stack allocation (default)

All local variables and struct instances live on the stack automatically:

```eskiu
int x = 42;
let p: Point = Point { x: 1.0, y: 2.0 };
```

### Heap allocation with alloc / free

`alloc<T>(n)` allocates `n` items of type `T` on the heap and returns `*T`;
`free(ptr)` releases it. Both come from the `<mem>` stdlib (they are ordinary
generic functions, not keywords):

```eskiu
import <mem>;
import <io>;

int main() {
    let buf: *int = alloc<int>(8);
    for (int i = 0; i < 8; i += 1) {
        *(buf + i) = i * i;
    }
    for (int i = 0; i < 8; i += 1) {
        printf("%d ", *(buf + i));
    }
    printf("\n");
    free(buf);
    return 0;
}
```

Output: `0 1 4 9 16 25 36 49`

### Null checks

```eskiu
import <io>;

int main() {
    let p: *int = null;
    if (p == null) {
        printf("pointer is null\n");
    }
    return 0;
}
```

For checked null safety, declare the pointer `?*T`. The compiler then rejects a
dereference until a check proves the pointer is not null. The check can be an
`if (p != null)`, an early exit (`if (p == null) { return 0; }`), a loop condition,
the left side of `&&` or `||`, or a ternary condition. Assigning to the pointer
ends what the check proved.

```eskiu
int first(?*int p) {
    if (p == null) { return -1; }
    return *p;                 // proven non-null here
}
```

### Dereference

```eskiu
int n = 100;
let ptr: *int = &n;
*ptr = 200;       // write through pointer
int val = *ptr;   // read through pointer, val == 200
```

---

## Multi-file Projects

Eskiu has two import forms:

```eskiu
import <result>;      // stdlib module, resolved by the compiler
import "utils.esk";   // local file, relative to the current file
```

`import <name>` looks up the module in the Eskiu installation's stdlib directory. No path required. `import "path"` is relative to the importing file.

Each file is parsed only once regardless of how many times it is imported.

### Example layout

```
project/
  main.esk
  utils.esk
```

`utils.esk`:

```eskiu
import <io>;

void greet(string name) {
    printf("Hello, %s!\n", name);
}

int clamp(int val, int lo, int hi) {
    if (val < lo) { return lo; }
    if (val > hi) { return hi; }
    return val;
}
```

`main.esk`:

```eskiu
import "utils.esk";

int main() {
    greet("Eskiu");
    int v = clamp(150, 0, 100);
    return 0;
}
```

Compile the entry point only; the compiler follows imports automatically:

```bash
eskiuc main.esk -o main
./main
```

### Using stdlib modules

Import stdlib modules with angle brackets, no path needed:

```eskiu
import <result>;
import <io>;
import <math>;
```

Available modules:

| Module              | Contents                                              |
| ------------------- | ----------------------------------------------------- |
| `<result>`   | `Result<T,E>`, `Ok<T,E>()`, `Err<T,E>()`             |
| `<list>`     | `List<T>`: `List_init`, `push`, `get`, `len`, `free`; `map`, `filter`, `reduce`, `find`, `any`, `all`, `contains`, `index_of`, `reverse`, `slice`, `unique`, `clear`, `pop`, `insert` |
| `<map>`      | `Map<V>`: string-keyed hash map (`Map_init`/`_at`/`_get`/`_free`); `HashMap<K,V>`: keyed on any type via `hash`/`eq` function pointers; both with `has`, `get_or`, `len`, `remove`, `keys`, `values`, `each`; `Set<T>`: hash set (`add`/`has`/`remove`/`len`/`each`/`items`) |
| `<bytes>`    | `Bytes`: growable, binary-safe byte buffer (`_init`/`_push`/`_append`/`_slice`/`_eq`/`_from_str`, plus base64 round-trip) |
| `<string>`   | `String`: `init`, `from`, `append`, `concat`, `cstr`, `len`, `free`, `starts_with`, `ends_with`, `trim`, `split`, `next_token`; `find`, `contains`, `replace`, `replace_all`, `split_str`, `lines`, `join`, `lower`, `upper`, `repeat`, `pad_left`, `pad_right`, `to_double`, `parse_double`, `from_double`, `appendf`; `List_join`, `String_format` |
| `<ctype>`    | ASCII character classes: `is_space`, `is_digit`, `is_hex`, `is_alpha`, `is_alnum`, `is_ident_start`, `is_ident_cont` |
| `<math>`     | `sqrt`, `fabs`, `pow`, `floor`, `ceil`, `abs`         |
| `<io>`       | `printf`, `fprintf`, `sprintf`, `scanf`, `puts`; `print`, `println`, `print_err`, `println_err`, `read_line`, `input`, `read_all_stdin` |
| `<mem>`      | `alloc<T>(n)`, `free(p)`, `memcpy`, `memset`, `memmove`, `memcmp`, `strlen` |
| `<ctypes>`   | `size_t`, `ssize_t`, `c_long`: C integer types for `extern` declarations, as wide as the target's C types (32 bits on wasm32 and 32-bit ARM and x86) |
| `<libc>`     | the common C library for ported code (opt-in): string.h, stdlib.h (`malloc`, `strtoll`, `qsort`, ...), stdio.h (`snprintf`, `sscanf`, `fopen`, ...), ctype.h, the rest of libm (`sin`, `log`, `round`, ...), POSIX unistd, `libc_errno()` |
| `<atomic>`   | `atomic_load`/`store`/`swap`/`cas`/`fetch_add`... on `int`, `int64` (`...64`) and pointer (`..._ptr`) cells, `..._relaxed` forms, `atomic_fence()` |
| `<alloc>`    | `Bump`, `Arena`, `Pool`, `FirstFit`: explicit allocators over a buffer you own (the Zig model; not a `malloc` replacement) |
| `<either>`   | sum types: `Option<T>`, `Either<A,B>` + helpers (built on generic algebraic enums) |
| `<futureval>`| value-returning combinators: `select2v` -> `Either`, `join2v` -> `Pair` |
| `<http2>`    | HTTP/2 (RFC 7540) wire layer: frame-header codec + constants, connection lifecycle (`H2Conn`), per-stream state machine + flow control (`H2Stream`), and async frame I/O |
| `<hpack>`    | HPACK (RFC 7541) header compression: static + dynamic tables, prefix-integer / string coding, Huffman |
| `<http2_server>`| HTTP/2 (h2c, cleartext) server over the event loop: `http2_serve_async`, multiplexed streams, same handler interface as `<http>` |
| `<tls>`      | h2 over TLS via OpenSSL with ALPN `"h2"`: `http2_tls_serve_conn` (blocking) / `http2_tls_serve_async` |
| `<sysheap>`  | `Heap`: a general heap that `mmap`s OS pages and runs `FirstFit` on them (no libc `malloc`) |
| `<fs>`       | `fs_open`, `fs_close`, `fs_flush`, `fs_read`, `fs_readline`, `fs_write`, `fs_puts`, `fs_seek`, `fs_tell`, `fs_size`, `fs_read_all`, `fs_write_all`, `fs_eof`, `fs_error`; `fs_read_text`, `fs_write_text`, `fs_append_text`, `fs_read_lines`, `fs_stat`, `fs_exists`, `fs_is_dir`, `fs_is_file`, `fs_mkdir`, `fs_mkdir_p`, `fs_remove`, `fs_remove_all`, `fs_rename`, `fs_copy`, `fs_list_dir`, `fs_walk`, `fs_glob`, `fs_cwd`, `fs_chdir`, `fs_temp_file`, `fs_temp_dir`, `fs_last_error` |
| `<net>`      | TCP sockets: `net_tcp_listen`, `net_accept`, `net_tcp_connect` (IPv4 address or host name), `net_send`, `net_recv`, `net_close` |
| `<dns>`      | `dns_resolve` (IPv4 and IPv6 addresses of a host), `dns_free_list`, `dns_last_error` |
| `<http>`     | HTTP/1.1: `HttpRequest`/`HttpResponse`, threaded `http_serve(port, workers, handler)` |
| `<fetch>`    | HTTP client for `http://` and `https://`: `fetch`, `fetch_with` + `FetchOptions` (method, headers, body, JSON body, timeout, redirects, body cap), `fetch_async`; `Response` with `Response_header`, `Response_text`, `Response_bytes`, `Response_json`, `Response_ok`; `fetch_last_error` |
| `<json>`     | `Json` builder + `json_parse` → `JsonValue` tree; build and edit with `JsonValue_object`/`_array`/`_set`/`_push`/`_remove`, print with `JsonValue_stringify`/`_pretty`, paths with `json_get` |
| `<csv>`      | RFC 4180 CSV: `csv_parse` into a `CsvTable` (`CsvTable_get(t, row, "column")`), `csv_write_row` |
| `<dotenv>`   | `.env` files: `dotenv_load`, `dotenv_load_override`, `dotenv_parse` |
| `<hash>`     | SHA-256, SHA-1, MD5, HMAC-SHA256 (streaming and one-shot), `hash_hex_encode`/`hash_hex_decode`, `hash_equal` |
| `<multipart>`| extract a named part from a `multipart/form-data` body: `multipart_boundary`, `multipart_part` |
| `<base64>`   | `base64_encode` / `base64_decode` over byte buffers |
| `<random>`   | seedable PRNG (xoshiro256\*\*): `Rng_seed`, `Rng_next`, `Rng_below`, `Rng_range`, `Rng_double`, `Rng_bool`, `Rng_fill` |
| `<regex>`    | linear-time regex (Thompson NFA) over UTF-8: `regex_match`, `regex_compile`/`Regex_search` with capture groups (`Match_group`), `Regex_search_at` from an offset, `Regex_iter` over every match, `\d \w \s`, `\p{L}` Unicode classes, `* + ? {m,n}`, `|`, `( )`, `(?:)`, `(?i)`, `^ $`, `\b \B`, `\Q..\E` (RE2 syntax, as Go's regexp) |
| `<sort>`     | generic in-place `sort<T>`, `sort_partial<T>`, `sort_nth<T>` (libc++'s algorithms, so ties order as in C++) + `sort_search<T>` over a `*T` array, via a `cmp(&x, &y)` function; `List_sort_by` sorts a `List<T>` |
| `<url>`      | RFC 3986 percent-encoding: `url_encode`, `url_decode`, form-query lookup `url_query_get`; `url_parse` into a `Url`, `Url_resolve`, `url_query_add` |
| `<uuid>`     | RFC 4122 v4 UUIDs: `uuid_v4(&rng, &out)` (built on `<random>`) |
| `<time>`     | `time_now_ms`, `time_now_s`, `time_monotonic_ms`, `time_sleep_ms`; UTC civil calendar (`DateTime`, `time_to_utc`/`DateTime_to_epoch`, `DateTime_format_iso`); local time, `time_format`/`time_parse` (strftime-style), `time_format_duration`, `Stopwatch` |
| `<env>`      | `env_get`, `env_has`, `env_get_or`, `env_get_int`, `env_set`, `env_unset`, `env_list` |
| `<process>`  | `process_run` (capture output, stdin text, cwd, env, timeout), `sh`, `sh_with`, `process_spawn` + `Process_write_stdin`/`_read`/`_wait`/`_kill`, `process_exit`, `process_pid` |
| `<args>`     | command-line parsing: `ArgParser`, `ArgParser_flag`, `ArgParser_option`, `ArgParser_option_int`, `ArgParser_argument`, generated `--help`, `ArgParser_parse_or_exit`, `ArgParser_get`/`ArgParser_get_int`/`ArgParser_get_flag`/`ArgParser_count` |
| `<term>`     | ANSI styles and colors (`TERM_BOLD \| TERM_RED`, `term_fg256`, `term_fg_rgb`), `term_style`, `term_println`, `term_is_tty`, `term_width`; honors `NO_COLOR`/`FORCE_COLOR` |
| `<log>`      | `log_info`/`log_warn`/`log_error`/`log_debug`/`log_trace` (printf-style), `LOG_LEVEL` or `log_set_level`, timestamps, `log_set_file` |
| `<path>`     | `path_join`, `path_basename`, `path_dirname`, `path_extension`, `path_is_absolute` |
| `<threading>`| `Mutex`, `Cond`, `Sem` over pthread (pairs with `thread_create`/`thread_join`) |
| `<eventloop>`| readiness reactor over kqueue/epoll: `el_new`/`EventLoop_add_read`/`EventLoop_run`/`EventLoop_stop`/`EventLoop_add_timer` |
| `<atomic>`   | atomic `int` cell: `atomic_load`/`atomic_store`/`atomic_swap`/`atomic_cas` |
| `<future>`   | the `async`/`await` runtime: `Future<T>`, `Future_poll`/`complete`/`drop`, `spawn`/`select2`/`join2` |
| `<executor>` | `Executor`: event loop + thread-safe ready-queue + self-pipe wakeup |
| `<net_async>`| async leaf futures: `net_read_async`, `net_accept_async` |
| `<timer>`    | `timer_after(lp, ms)`: a `*Future<int>` that completes after a delay (timeouts) |
| `<channel>`  | async message channel: `chan_new`/`Chan_send`/`Chan_recv` (a `*Future<T>`) |
| `<http_async>`| non-blocking concurrent HTTP/1.1 server: `http_serve_async` |

Modules that need a system library link it themselves (`<math>` adds `-lm` on
Linux through `#pragma link("m")`), so a program using them builds with no `-l`
flag:

```bash
eskiuc file.esk -o file
```

### Standard library highlights

A few of the modules. See the [language spec §14](spec.md#14-stdlib) for the full reference.

**`<alloc>`: allocators over a caller buffer.** `alloc_with(&a, T, n)` carves a
typed `*T` out of any struct that exposes a `_alloc` method. Bump is the
simplest: individual frees are no-ops; one `free` of the backing buffer releases
everything.

```eskiu
import <mem>;
import <alloc>;
import <io>;

int main() {
    *uint8 backing = alloc<uint8>(4096);
    let a: Bump;  Bump_init(&a, backing, 4096);
    *int xs = alloc_with(&a, int, 16);     // 16 ints from the slab, no per-object malloc
    xs[0] = 7;  xs[15] = 9;
    printf("%d %d\n", xs[0], xs[15]);
    free(backing);                          // frees xs too
    return 0;
}
```

**`<json>`: build and parse.** The builder inserts separators automatically;
`json_parse` returns a `*JsonValue` tree.

```eskiu
import <json>;
import <io>;

int main() {
    let j: Json;  Json_init(&j);
    Json_obj_begin(&j);
        Json_key(&j, "year");  Json_int(&j, 2026);
    Json_obj_end(&j);
    printf("%s\n", Json_cstr(&j));                 // {"year":2026}

    *JsonValue v = json_parse(Json_cstr(&j));
    printf("year=%lld\n", JsonValue_as_int(JsonValue_get(v, "year")));
    JsonValue_free(v);  Json_free(&j);
    return 0;
}
```

**`<http>`: a concurrent server in a few lines.** The handler fills the
response; `http_serve` runs a pool of worker threads over `<net>` + `<threading>`.

```eskiu
import <http>;

void handle(HttpRequest* req, HttpResponse* res) {
    HttpResponse_header(res, "Content-Type", "text/plain");
    HttpResponse_set_body(res, "Hello from Eskiu\n");
}

int main() { http_serve(8080, 4, handle); return 0; }   // 4 worker threads
```

### Fetching a URL

`<fetch>` is an HTTP/1.1 client. `fetch(url, &r)` does a GET and returns 0 once a
response arrived (any status), or a negative error code; `fetch_last_error()` says
what went wrong. Free the response with `Response_free` either way.

```eskiu
import <fetch>;
import <io>;

int main() {
    let r: Response;
    defer Response_free(&r);
    if (fetch("https://api.github.com/repos/llvm/llvm-project", &r) != 0) {
        printf("error: %s\n", fetch_last_error());
        return 1;
    }
    printf("HTTP %d, %lld bytes\n", r.status, r.body.len);
    *JsonValue repo = Response_json(&r);            // null if the body is not JSON
    if (repo != null) {
        printf("stars: %lld\n", JsonValue_as_int(JsonValue_get(repo, "stargazers_count")));
        JsonValue_free(repo);
    }
    return 0;
}
```

For a POST, headers or a timeout, fill a `FetchOptions` and call `fetch_with`:

```eskiu
let o: FetchOptions;  FetchOptions_init(&o);
defer FetchOptions_free(&o);
FetchOptions_method(&o, "POST");
FetchOptions_header(&o, "Authorization", "Bearer abc");
FetchOptions_body_text(&o, "{\"name\":\"eskiu\"}");
o.timeout_ms = 5000;
int rc = fetch_with("https://example.com/api/items", &o, &r);
```

Redirects are followed (up to 10). HTTPS loads OpenSSL (libssl 3 or 1.1) the first
time it is needed, so a program that only fetches `http://` URLs runs without it; on
macOS install it with `brew install openssl@3`, on Debian or Ubuntu it is the `libssl3`
package (usually present already).

### Scripting

With `eskiuc run` and a `#!/usr/bin/env -S eskiuc run` line (see
[Run it like a script](#run-it-like-a-script)), the modules above cover what small
tools usually need: `<fs>` for files and directories, `<process>` to run commands,
`<fetch>` for HTTP, `<json>`, `<csv>`, `<dotenv>` and `<hash>` for data, `<args>`,
`<term>` and `<log>` for the command line, and `<time>` for dates. This script
counts the lines of each `.esk` file under a directory, largest first:

```eskiu
#!/usr/bin/env -S eskiuc run
import <args>;
import <fs>;
import <sort>;

struct Count { String path; int lines; }

int main(int argc, string* argv) {
    let p: ArgParser;
    ArgParser_init(&p, "count_lines", "Count the lines of the .esk files under DIR.");
    ArgParser_argument(&p, "dir", "where to look", ARGS_REQUIRED);
    ArgParser_parse_or_exit(&p, argc, argv);

    let pattern: String;  pattern.from(ArgParser_get(&p, "dir"));
    pattern.append("/**/*.esk");
    let files: List<String>;  List_init<String>(&files, 16);
    fs_glob(pattern.cstr(), &files);

    let counts: List<Count>;  List_init<Count>(&counts, 16);
    let lines: List<String>;  List_init<String>(&lines, 64);
    for (i in 0..files.size) {
        if (fs_read_lines(files.data[i].cstr(), &lines) == 0) {
            counts.push(Count { path: files.data[i], lines: lines.size });
        }
        fs_list_free(&lines);
        List_init<String>(&lines, 64);
    }
    counts.sort_by(int(*Count a, *Count b) { return b.lines - a.lines; });
    for (i in 0..counts.size) { printf("%6d  %s\n", counts.data[i].lines, counts.data[i].path.cstr()); }
    return 0;
}
```

`examples/scripts/` has two longer ones: `repo_stats.esk` (the GitHub API through
`<fetch>`, a colored summary or JSON) and `organize.esk` (sorting a directory's files
into folders, with duplicates found by SHA-256 and a CSV report). Chapter 25 of
[The Book of Eskiu](https://eskiu-lang.org/the-book-of-eskiu.html#chapter-25) walks
through every module with a runnable example.

---

## Packages

A package is a directory with an `eskiu.toml` manifest. `eskiuc new` creates one:

```bash
eskiuc new myapp
cd myapp
eskiuc run
```

```
myapp/
  eskiu.toml
  src/main.esk
  .gitignore
```

The manifest names the package and lists its dependencies:

```toml
[package]
name = "myapp"
version = "0.1.0"
main = "src/main.esk"      # optional, this is the default

[deps]
jsonx = { git = "https://github.com/x/jsonx", rev = "v1.2.0" }   # a tag, branch or commit
util  = { path = "../util" }
```

`eskiuc add` edits the manifest for you and fetches the dependency:

```bash
eskiuc add jsonx --git https://github.com/x/jsonx --rev v1.2.0
eskiuc add util --path ../util
```

A git dependency is cloned with the `git` command into `~/.eskiu/pkgs/NAME/COMMIT`
(`$ESKIU_HOME/pkgs` when `ESKIU_HOME` is set). Without `--rev` it follows the remote's
default branch. A path dependency is used where it is. A dependency is a directory of
`.esk` files; when it has its own `eskiu.toml`, its dependencies are fetched too. Two
packages that need the same dependency at different commits are an error that names both.

The resolved commits go to `eskiu.lock`, which you commit next to `eskiu.toml`. Later
builds use the locked commits and run no git command when the checkouts are already
there. `eskiuc fetch` resolves every rev again and updates the lock.

Import a module of a dependency by the package name and the module's file name:

```eskiu
import <jsonx/parse>;   // parse.esk, or src/parse.esk, in the jsonx package
import <util>;          // util.esk, src/util.esk or lib.esk in the util package
import <myapp/helper>;  // the package's own src/helper.esk
```

A stdlib module keeps its name: `import <io>;` is always the stdlib one. An import that
names neither a stdlib module nor a package in the lock is an error:
`unknown package 'x' (not in eskiu.toml)`.

### Native libraries

A package that binds a C library can ship it: a `[native.NAME]` table lists one prebuilt
archive per platform for the library that `#pragma link("NAME")` (or `-lNAME`) links, with
its SHA-256 and the directory inside it that holds the libraries.

```toml
[native.onnxruntime]
macos-arm64  = { url = "https://github.com/microsoft/onnxruntime/releases/download/v1.31.0/onnxruntime-osx-arm64-1.31.0.tgz",
                 sha256 = "1031b36d...", lib = "onnxruntime-osx-arm64-1.31.0/lib" }
linux-x86_64 = { url = "...", sha256 = "...", lib = "onnxruntime-linux-x64-1.31.0/lib" }
```

The platforms are `macos-arm64`, `macos-x86_64`, `linux-x86_64`, `linux-arm64`,
`windows-x86_64`, `windows-arm64`, `ios-arm64`, `ios-arm64-simulator`,
`ios-x86_64-simulator`, `android-arm64` and `android-x86_64`; `lib` defaults to `lib`. The
first build for a platform that links the library, inside the package or a package that
depends on it (by `eskiuc build`, `run`, `test`, `eskiuc file.esk`, or a `--target` cross
build), downloads the archive with `curl`, refuses it when its SHA-256 differs, and unpacks
it (`tar`, or `unzip` for a `.zip` on Linux) into `~/.eskiu/native/SHA256`, once;
`eskiuc fetch` downloads every host one ahead. The build then links against that directory
and records it as the program's run-time path; on Windows the DLLs are copied next to the
program. A program that does not link the library downloads nothing, and a platform the
table leaves out links it from the system as before. Only the C++ `eskiuc` reads the table.

The [eskiu-lang](https://github.com/eskiu-lang) organization publishes packages that are
added this way: [gpu](https://github.com/eskiu-lang/gpu) (WebGPU compute, 3D and machine learning),
[onnx](https://github.com/eskiu-lang/onnx), [tflite](https://github.com/eskiu-lang/tflite),
[compress](https://github.com/eskiu-lang/compress), [audio](https://github.com/eskiu-lang/audio),
[font](https://github.com/eskiu-lang/font) and [nds](https://github.com/eskiu-lang/nds).

The package commands:

```bash
eskiuc build [-o out] [flags]   # compile src/main.esk to build/NAME
eskiuc run [args]               # build, then run with args
eskiuc test                     # compile and run every tests/*.esk, pass = exit code 0
eskiuc fetch                    # re-resolve the dependencies, rewrite eskiu.lock
```

`eskiuc run file.esk` still compiles and runs that one file, inside a package or not, and
compiling any file of a package (`eskiuc src/tool.esk -o tool`) resolves package imports
from the lock. The self-hosted `eskiuc-esk` reads the lock the same way; the package
commands themselves are in the C++ `eskiuc` only.

---

## Lambdas

Eskiu supports anonymous functions (lambdas) as first-class values. The syntax
mirrors a regular function declaration without a name.

### Function pointer types

Use the `fn(T,...)->R` syntax to declare a function pointer type:

```eskiu
let f: fn(int)->int;
let g: fn(int, int)->bool;
```

`fn` names a *type* only. It is not how you define a function (Eskiu has no `fn name(...)` form): functions are defined C-style, return type first, as in `int add(int a, int b) { ... }`.

### Writing a lambda

```eskiu
import <io>;

int main() {
    let double_it: fn(int)->int = int(int x) { return x * 2; };
    printf("%d\n", double_it(5));   // 10
    return 0;
}
```

### Passing lambdas as arguments

```eskiu
import <io>;

int apply(fn(int)->int f, int x) {
    return f(x);
}

int main() {
    let square: fn(int)->int = int(int n) { return n * n; };
    printf("%d\n", apply(square, 6));        // 36
    printf("%d\n", apply(int(int n) { return n + 1; }, 9));  // 10
    return 0;
}
```

### Closures: capturing from the enclosing scope

A lambda can reference variables declared in the surrounding scope. Those variables are captured by value at the point the lambda is created. The lambda holds its own copy, so it may not assign one (`x = 1`, `x++` inside the body is a compile error) or call a method on it that takes a plain `*T self` (only a `const T* self` method); to share state, write through a pointer or use a global or a `static` local.

```eskiu
import <io>;

int main() {
    int base = 10;
    let add: fn(int)->int = int(int x) { return x + base; };
    printf("%d\n", add(5));   // 15
    printf("%d\n", add(7));   // 17
    return 0;
}
```

Closures can be passed to higher-order functions exactly like plain lambdas: the `fn(T)->R` type is the same in both cases.

```eskiu
import <io>;

int apply(fn(int)->int f, int x) { return f(x); }

int main() {
    int offset = 100;
    let shift: fn(int)->int = int(int x) { return x + offset; };
    printf("%d\n", apply(shift, 5));   // 105
    return 0;
}
```

Under the hood, `fn(T)->R` is a fat pointer `{fn_ptr, env_ptr}`. Non-capturing lambdas have `env_ptr = null` and behave like plain function pointers.

---

## Threads

`thread_create` and `thread_join` are language keywords. Pass any `fn()->void` value, including a closure, to spawn a thread.

### Basic usage

```eskiu
import <io>;

int main() {
    *void t = thread_create(void() { printf("hello from thread\n"); });
    thread_join(t);
    return 0;
}
```

### Capturing outer state

```eskiu
import <io>;

int main() {
    int id = 42;
    let worker: fn()->void = void() { printf("thread id = %d\n", id); };
    *void t = thread_create(worker);
    thread_join(t);
    return 0;
}
```

A closure value's fat pointer maps directly to pthread's `(start_routine, arg)` pair. A lambda written in the call goes through a small trampoline that frees its environment when the thread ends. The driver links pthread by itself where the platform needs it:

```bash
eskiuc threads.esk -o threads
./threads
```

---

## Exception Handling

Use `try`, `catch`, `finally`, and `throw` for structured exception handling.

### Basic try/catch

```eskiu
import <io>;

int divide(int a, int b) {
    if (b == 0) {
        throw "division by zero";
    }
    return a / b;
}

int main() {
    try {
        int r = divide(10, 0);
        printf("result: %d\n", r);
    } catch (string e) {
        printf("caught: %s\n", e);
    }
    return 0;
}
```

Output: `caught: division by zero`

### With finally

The `finally` block always executes, whether or not an exception was raised, and
also when the `try` body or a `catch` handler leaves early with `return`, `break` or
`continue`:

```eskiu
import <io>;

int main() {
    try {
        throw "error";
    } catch (string e) {
        printf("caught: %s\n", e);
    } finally {
        printf("cleanup\n");
    }
    return 0;
}
```

Output:
```
caught: error
cleanup
```

### Linking

Exception handling uses the Itanium C++ ABI personality function, from the platform C++ runtime. The driver links it when the program uses `throw` or `try` (`-lc++` on macOS, `-lstdc++` on Linux and Windows), so no flag is needed:

```bash
eskiuc file.esk -o file
./file
```

---

## IDE Tooling (VS Code)

The `editor/vscode/` directory contains a VS Code extension for Eskiu.

### Install

Package the extension and install the `.vsix` (from the repository root):

```bash
cd editor/vscode
npx @vscode/vsce package --allow-missing-repository --skip-license
code --install-extension eskiu-language-*.vsix
```

For development, link the folder instead: `ln -s "$PWD/editor/vscode" ~/.vscode/extensions/eskiu-language`, then reload the window. The extension uses the `eskiuc` on your `PATH`. See `editor/vscode/README.md` for details.

### Features

| Feature | How it works |
|---------|-------------|
| Syntax highlighting | TextMate grammar (`eskiu.tmLanguage.json`) |
| Real-time error squiggles | Extension runs `eskiuc --test-typechecker` as you type (on the unsaved buffer) and on save, and parses the `file:line:col: message` output |
| Hover type info | Extension calls `eskiuc --hover-at LINE:COL` and shows the result in a tooltip |
| Go-to-definition | Extension calls `eskiuc --definition-at LINE:COL` and jumps to the returned location |

### CLI flags used by the extension

```bash
# Print the inferred type of the expression at line 10, column 5
eskiuc file.esk --hover-at 10:5

# Print the definition location of the symbol at line 10, column 5
eskiuc file.esk --definition-at 10:5
```

Both flags accept `LINE:COL` with 1-based line and column numbers and print a
single line of output. They can also be used directly on the command line for
scripting.

---

## Using the Test Modes

The compiler exposes diagnostic flags that stop compilation after a specific
phase and print what was produced. None of them produce an object file. They
predefine the same macros as a real build (so `#ifdef __APPLE__` picks the same
branch), and each exits with a non-zero status when it reports an error.

| Flag                      | Phase        | Output                       | When to use                                   |
| ------------------------- | ------------ | ---------------------------- | --------------------------------------------- |
| `--test-lexer`            | Lexer        | Token stream with line/col   | Debugging unexpected parse errors             |
| `--test-parser`           | Parser       | Indented AST                 | Checking whether syntax is parsed correctly   |
| `--test-typechecker`      | Type checker | Errors, then `Type checking succeeded!` / `failed!` | Catching type mismatches before codegen |
| `--test-codegen`          | Code gen     | LLVM IR                      | Inspecting what IR the compiler produces      |
| `--hover-at LINE:COL`     | Type checker | Type at position             | Hover type info (used by VS Code extension)   |
| `--definition-at LINE:COL`| Type checker | Definition location          | Go-to-definition (used by VS Code extension)  |

### --test-lexer

```bash
eskiuc hello.esk --test-lexer
```

Produces one token per line with its kind, text, and `line:col` position.
Useful when you see a parse error and want to check if the tokenizer is splitting
tokens correctly.

### --test-parser

```bash
eskiuc hello.esk --test-parser
```

Prints the full AST as an indented tree. Each node shows its kind and the
relevant identifiers or literals. Use this to verify that operator precedence,
struct literals, and template arguments are parsed the way you expect.

### --test-typechecker

```bash
eskiuc hello.esk --test-typechecker
```

Runs type inference and struct field validation. If a struct field name is wrong
you see the error here, before any IR is generated:

```
error: hello.esk:5:14: struct 'Point' has no member 'z'
```

### --test-codegen

```bash
eskiuc hello.esk --test-codegen
```

Prints the full LLVM IR to stdout. Useful for verifying that a cast, bitwise
operation, or pointer dereference lowered the way you intended.

---

## Inline Assembly and volatile

These features are primarily useful for bare-metal and kernel programming. Both are available in any Eskiu program.

### volatile

Mark a pointer variable `volatile` to prevent the compiler from optimising away loads and stores through it. This is essential for memory-mapped I/O (MMIO) registers:

```eskiu
volatile let uart: *uint8 = (uint8*) 0x3F8;

// Both the read and the write are always emitted, never cached or removed
uint8 status = *uart;
*uart = 'A';
```

Without `volatile`, the compiler may eliminate or reorder accesses to addresses it considers side-effect-free.

### Inline assembly: simple form

```eskiu
asm("cli");   // disable interrupts
asm("sti");   // enable interrupts
asm("nop");
```

The string is passed verbatim to the assembler. Use this form for instructions that take no operands.

### Inline assembly: extended form

The extended form passes values in and out of the asm template. Its layout and constraints follow GCC:

```eskiu
// Write a byte to an x86 I/O port
void outb(uint8 val, uint16 port) {
    asm("outb ${0:b}, $1" :: "a"(val), "Nd"(port) : "memory");
}

// Add two registers on x86-64 and read the result back
int64 add(int64 a, int64 b) {
    int64 acc = a;
    asm("addq $1, $0" : "+r"(acc) : "r"(b));
    return acc;
}
```

Syntax: `asm("template" : outputs : inputs : clobbers);` (empty sections may be left blank, as in `::`).

- Operands are `"constraint"(expression)` pairs, referenced in the template as `$0`, `$1`, ... (outputs first, then inputs).
- An output constraint starts with `=` (written) or `+` (read and written), and its operand must be a writable variable, field, element or dereference.
- `"memory"` in the clobber list acts as a compiler barrier.
- Common constraints: `"r"` → any register, `"m"` → memory, `"i"` → an immediate. On x86 the GCC register letters `"a"`, `"b"`, `"c"`, `"d"`, `"S"` and `"D"` pick one register (al/ax/eax/rax by width for `"a"`), and `"Nd"` is an 8-bit immediate or dx. LLVM's explicit form, a register in braces such as `"{ax}"` or `"N{dx}"`, works too; the GCC letters are translated to it.

See spec §15 for the full rules.

### Freestanding mode

When targeting bare metal or a kernel, pass `--freestanding` to the compiler. This makes `<mem>`'s `alloc<T>` and `free` call user-provided `esk_alloc` / `esk_free` symbols instead of libc `calloc` / `free`:

```bash
eskiuc kernel.esk --target x86_64-pc-linux-gnu --freestanding -o kernel.o
```

You must provide `esk_alloc` and `esk_free` in your own source or a C shim:

```eskiu
// kernel_alloc.esk, linked together with kernel.esk
*void esk_alloc(int64 nbytes) { return bump_alloc(nbytes); }   // must return zeroed memory
void  esk_free(*void ptr)  { bump_free(ptr); }
```

### Safe mode

Pass `--safe` to insert runtime safety checks. It bounds-checks slice and fixed-array
indexing (an out-of-range index traps instead of reading or writing past the end), traps
on a signed `int`/`int64` `+ - *` that overflows, and traps on a null pointer dereference.
`--safe=LIST` turns on only some of them: `--safe=bounds`, `--safe=overflow,null`. The
checks are **off by default**, so release builds carry no overhead; turn them on for
development and test runs.

```bash
eskiuc app.esk --safe -o app     # a[i] / s[i] out of range now aborts instead of corrupting memory
```

```eskiu
int[5] a = {1, 2, 3, 4, 5};
int[] s = a[1..4];               // length 3
int x = s[9];                    // under --safe: aborts here; without it: reads garbage (as in C)
```

---

## sizeof

`sizeof(T)` is a compile-time constant expression that returns the size of a type in bytes as an `int64`. It works for any Eskiu type, including structs.

```eskiu
import <io>;

struct Grid {
    float x;
    float y;
    float z;
}

int main() {
    printf("sizeof(int)    = %lld\n", sizeof(int));    // 4
    printf("sizeof(int64)  = %lld\n", sizeof(int64));  // 8
    printf("sizeof(Grid)   = %lld\n", sizeof(Grid));   // 12
    return 0;
}
```

`sizeof` is resolved entirely at compile time and produces no runtime code. As in C, `sizeof(x)` where `x` names a variable gives the size of that variable's type (`let d: double; sizeof(d)` is 8). A name that is neither a type nor a variable is an error.

---

## Unions

A `union` is declared like a `struct`, but all fields share offset 0. The size of the union equals the size of its largest field, rounded up to the alignment of its most-aligned field, as in C. Reading a field reinterprets the stored bytes as that field's type.

```eskiu
import <io>;

union Value {
    int   i;
    float f;
}

int main() {
    let v: Value;
    v.i = 0x3F800000;            // IEEE 754 bit pattern for 1.0
    printf("%f\n", v.f);         // 1.000000, same bytes, read as float
    v.f = 3.14;
    printf("%d\n", v.i);         // raw integer bits of 3.14
    return 0;
}
```

---

## Typed Pointer Arithmetic

Pointer arithmetic scales by the pointed-to type's size, matching C behaviour. Adding `1` to a `*int` advances 4 bytes; adding `1` to a `*Grid` advances `sizeof(Grid)` bytes.

```eskiu
import <mem>;
import <io>;

int main() {
    let buf: *int = alloc<int>(4);
    *buf = 10;
    *(buf + 1) = 20;   // +4 bytes (one int)
    *(buf + 2) = 30;   // +8 bytes
    *(buf + 3) = 40;   // +12 bytes
    for (int i = 0; i < 4; i += 1) {
        printf("%d\n", *(buf + i));
    }
    free(buf);
    return 0;
}
```

`*void` and `*char` are exceptions: they always step one byte at a time.

Subtracting two pointers of the same type gives the number of elements between
them, as in C: with `let q: *int = buf + 3;`, `q - buf` is `3`.

---

## What's Next

- Full language reference: [spec.md](spec.md)
- Formal grammar: [grammar.md](grammar.md)
- Standard library source: [`stdlib/`](../../stdlib/)
- Worked examples: [`examples/`](../../examples/)
- Contributing guide: [contributing.md](../dev/contributing.md)
