# Eskiu Language Reference

Eskiu is a statically typed systems language with the power of C and the immediacy of a scripting language: it compiles to native code through LLVM, and `eskiuc run file.esk` runs a source file directly, like Python or Ruby. This is the full language reference. New to Eskiu? Start with the [tutorial](getting-started.md).

**Current version: v0.14.0**; 0.13.0 was the first public release. Code that compiles with 0.14.0 keeps
compiling in every 0.14.x, and later changes are announced by warnings first
([stability.md](stability.md)). What the release contains is in the
[changelog](../../CHANGELOG.md).

---

## Documentation Map

| Document | What it covers | Start here if… |
|---|---|---|
| [getting-started.md](getting-started.md) | Hands-on tutorial: build, hello world, all features | You're new to Eskiu |
| [spec.md](spec.md) | Complete language reference | You need exact syntax or semantics |
| [grammar.md](grammar.md) | Formal EBNF grammar | You need the precise concrete syntax |
| [build.md](build.md) | Build the compiler from source on macOS, Linux and Windows | You need to set up a dev environment |
| [stability.md](stability.md) | What can change between releases, and how renames are deprecated | You maintain code across Eskiu versions |
| [glossary.md](glossary.md) | Terminology definitions | You encounter an unfamiliar term |

---

## Feature Summary

| Category | Features |
|---|---|
| **Types** | `int`, `int8`–`int64`, `uint`–`uint64`, `float`, `double`, `bool`, `char`, `string`, `void`, `*T` pointers, `?*T` checked nullable pointers, `T[N]` fixed arrays, `T[]` slices, `union`, `type` aliases |
| **Literals** | Decimal, hex `0xFF`, octal `0755`, float (`1.5`, `2.5e-3`), string (with adjacent concat), char, bool, null; no suffixes (`3.5f`, `1_000` are errors) |
| **Operators** | Arithmetic `+ - * / %`, bitwise `& \| ^ ~ << >>`, comparison, logical, compound `+= -=` etc., `++`/`--`, ternary `c ? a : b`, typed pointer arith `ptr+n` (advances by `sizeof(*ptr)`), `sizeof(T)`, user-defined `operator` overloads |
| **Control flow** | `if`/`else`, `for` (with decl init), `for (x in ...)`, `while`, `do`/`while`, `switch`/`case`, `match`, `break`/`continue` (optionally labeled), `return`, `defer`/`errdefer` |
| **Functions** | C-style, `extern` C ABI (functions and global variables), variadic, template `fn<T>(T x)` |
| **Lambdas / Closures** | `int(int x) { return x * 2; }`: anonymous functions; `fn(T,...)->R` fat-pointer types; closure capture by value; higher-order functions |
| **Threads** | `thread_create(fn()->void)` / `thread_join(*void)`: OS thread keywords; closure fat pointer maps directly to pthread ABI |
| **Exceptions** | `try`/`catch`/`finally`/`throw`: LLVM `invoke`/`landingpad`, Itanium ABI; multiple `catch` clauses; the driver links the C++ runtime |
| **Async / await** | `async` function lowered to a resumable state-machine coroutine; a call yields `*Future<T>`; `await` suspends until ready. Runtime in `<future>`/`<executor>`/`<eventloop>` |
| **Enums / match** | `enum` of named integer constants; payload-bearing variants form algebraic data types (tagged unions), destructured with exhaustive `match`; generic enums (`Option<T>`, `Either<A,B>`) |
| **Structs** | Fields, methods with `self`, struct literal init `Point { x: 1, y: 2 }`, bitfields `f: N`, `packed` layout |
| **Templates** | `struct Result<T,E>`, `fn Ok<T,E>(T v)`: monomorphic instantiation; bounded generics `<T: Iface>` / `<T: A + B>` (a primitive satisfies a constraint via a free function) |
| **Collections** | `<list>` `List<T>`, `<map>` `Map<V>` (string-keyed) / `HashMap<K,V>`, `<string>` `String`, `<bytes>` binary-safe `Bytes` buffer |
| **Interfaces** | `interface Drawable { void draw(); }`: structural typing (method signatures must match), vtable dispatch; an interface value (`let d: Drawable = &c;`) can be a local, field, parameter or return value |
| **Memory** | Stack default; heap via `<mem>` `alloc<T>(n)` / `free`; explicit allocators `<alloc>` via `alloc_with`; typed pointer arithmetic |
| **volatile** | `volatile let reg: *uint8 = (uint8*) 0x3F8;`: MMIO-safe loads/stores |
| **Inline asm** | `asm("cli");` simple form; `asm("outb ${0:b}, $1" :: "{ax}"(v), "N{dx}"(p) : "memory");` extended form (LLVM operand references `$0`/`$1` and LLVM constraint codes) |
| **Freestanding** | `--freestanding` flag: `<mem>` `alloc<T>`/`free` call `esk_alloc`/`esk_free`; user-supplied in kernel |
| **Cross-compile** | `--target TRIPLE` (AArch64, x86-64, 32-bit ARM, 32-bit x86, WebAssembly), plus `--mcpu` / `--mattr` / `--reloc`; hard-float ARM for the 3DS, COFF for Windows, emscripten and WASI, Android through the NDK, iOS through Xcode; `.a` and `.so` library outputs and `--subsystem=windows`. See [cross-compile.md](../dev/cross-compile.md) |
| **Multi-file** | `import <result>` stdlib modules · `import "file.esk"` relative local files |
| **Preprocessor** | `#define` (object- and function-like), `#undef`, `#ifdef`/`#ifndef`, `#if`/`#elif` with `defined(X)`, `#else`/`#endif`, `#error`, `#pragma pack`, `#pragma link`; predefined OS and architecture macros |
| **Errors** | `error: file.esk:8:22: message` for lexer, preprocessor, parser and type errors, naming the file the error is in |
| **HTTP/2** | `<http2>` frame codec + connection/stream state machine, `<hpack>` header compression, `<tls>` (h2 over OpenSSL with ALPN), `<http2_server>` (h2c): full multiplexed HTTP/2 stack |
| **Stdlib** | Core (`<result>`, `<list>`, `<string>`, `<math>`, `<io>`, `<mem>`, `<fs>`, `<path>`, `<base64>`, `<json>`, `<time>`, `<env>`); memory (`<alloc>`, `<sysheap>`); concurrency (`<threading>`, `<atomic>`); the async runtime (`<eventloop>`, `<future>`, `<executor>`, `<net_async>`, `<timer>`, `<channel>`, `<futureval>`); sum types (`<either>`); networking (`<net>`, `<http>`, `<http_async>`); and the HTTP/2 stack (`<http2>`, `<hpack>`, `<tls>`, `<http2_server>`) |

---

## Quick Reference

### Type → LLVM

| Eskiu | LLVM | Eskiu | LLVM |
|---|---|---|---|
| `int` / `int32` | `i32` | `uint` / `uint32` | `i32` |
| `int8` | `i8` | `uint8` | `i8` |
| `int16` | `i16` | `uint16` | `i16` |
| `int64` | `i64` | `uint64` | `i64` |
| `float` | `float` | `double` | `double` |
| `bool` | `i1` | `char` | `i8` |
| `string` | `ptr` | `*T` / `T*` | `ptr` |

### Operator precedence (lowest → highest)

```
assignment  = += -= *= /= %= &= |= ^= <<= >>=
ternary     ?:
logical     || &&
bitwise     | ^ &
equality    == !=
relational  < > <= >=
shift       << >>
additive    + -
multiplicative * / %
unary       ! - ~ & * ++ -- (TYPE)
postfix     f() a[i] a.b ++ -- ?
```

### CLI flags

| Flag / subcommand | Action |
|---|---|
| `-o prog` / `-o file.o`     | Link an executable / emit an object file (by suffix) |
| `-c`                        | Compile to an object file only                  |
| `-O0` / `-O1` / `-O2` / `-O3` | Optimization level (default `-O0`; `-O1`+ run the LLVM middle-end) |
| `-l<lib>` / `-L<path>`      | Pass library / search-path flags to the linker  |
| `--link-arg=ARG`            | Pass an extra argument to the linker            |
| `--no-default-libs`         | Link only the `-l` libraries given (no `#pragma link`, no implied runtimes) |
| `eskiuc run file.esk [-- args]` | Compile to a temp executable and run it     |
| `eskiuc fmt [--check] file.esk` | Format source in place                      |
| `-Wall` / `-Wextra`         | Lint warnings / signed-unsigned comparison warnings |
| `--asan` / `--ubsan`        | AddressSanitizer / trapping bounds checks       |
| `--safe[=LIST]`             | Run-time checks that trap: `bounds`, `overflow`, `null` (all without a list) |
| `--target TRIPLE`           | Cross-compile for the given target triple        |
| `--mcpu` / `--mattr` / `--reloc` | Target CPU, feature string, relocation model |
| `--freestanding`            | Use `esk_alloc`/`esk_free` instead of libc      |
| `--test-lexer`              | Print token stream                              |
| `--test-parser`             | Print AST                                       |
| `--test-typechecker`        | Type check and report errors                    |
| `--test-codegen`            | Print LLVM IR                                   |
| `--hover-at LINE:COL`       | Print inferred type of expression at position   |
| `--definition-at LINE:COL`  | Print definition location of symbol at position |
| `--version`                 | Print version                                   |
