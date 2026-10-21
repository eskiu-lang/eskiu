# Changelog

All notable changes to Eskiu are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
project follows the compatibility rules in [docs/lang/stability.md](docs/lang/stability.md).

---

## [Unreleased]

### Fixed
- A lambda in a generic function, passed to a parameter that does not escape, gets its
  env on the stack as it does elsewhere. It got a heap env that was never freed, so each
  `sort<T>`, `sort_partial<T>` and `sort_nth<T>` call leaked 16 bytes.
- The self-hosted compiler gave a stack env to a lambda passed to a generic function that
  keeps its closure parameter, which then read a dead stack frame; it now decides as the
  C++ compiler does. It also rejects forwarding a non-`escaping` closure parameter to a
  generic function, as the C++ compiler does.
- The self-hosted compiler captures a variable a lambda names only inside `free_closure`,
  `thread_create` or `alloc_with`; it emitted invalid IR for such a lambda.

## [0.12.0] - 2026-10-20

The first public release. Code that compiles with 0.12.0 keeps compiling in every 0.12.x
([stability.md](docs/lang/stability.md)).

### The language
- C syntax and C integer rules: promotions, the usual arithmetic conversions, `bool` as
  `!= 0`, element-counting pointer subtraction, `sizeof` of a type or an expression, octal,
  hex and binary literals with `_` digit separators.
- Run-time integer behavior is defined at every optimization level: overflow wraps, a
  shift count is taken modulo the width, a zero divisor traps, `MIN / -1` wraps, and float
  to integer saturates.
- Structs with methods and dot syntax, field defaults, unions, bitfields and packed
  structs laid out as the target's C compiler lays them out; enums whose members are
  constant expressions; type aliases.
- Generics with inferred type arguments and interface bounds (`<T: Ord>`), generic structs
  with inline methods, and dot calls on generic instances (`list.push(8)`).
- Structural interfaces as first-class values, sum types with exhaustive `match`, closures
  with escape analysis, operator overloading, labeled `break`/`continue`, `?:`,
  `do`/`while`, `static` locals and multidimensional arrays.
- Default parameter values (`void connect(string host, int port = 443)`, struct literals
  of constants included: `V3 up = V3{0.0, 1.0, 0.0}`) and `dbg(e)`,
  which prints an expression's source and value to stderr and yields the value.
- `deprecated("use X")` on functions, and "did you mean" hints on misspelled names.
- Scripts: a file may have statements at the top level, which run in order as an implicit
  `main` with the arguments in `argc` and `argv`; its top-level variables are globals the
  functions can see.

### Memory and safety
- No garbage collector: `alloc<T>` (zeroed memory) and `free`, and explicit allocators in
  `<alloc>` (bump, arena, pool, first fit) and `<sysheap>`.
- `defer` and `errdefer`, slices (`T[]`), checked nullable pointers (`?*T`) with
  flow-sensitive narrowing, `must_use`, `const` receivers, and an opt-in `--safe` build
  that traps on out-of-bounds indexes, signed overflow and null dereferences, each
  category on its own with `--safe=bounds,overflow,null`.
- Locals, `alloc_with` blocks and sum-type values start at zero.
- Warnings on by default for common mistakes (string comparison by address, printf
  formats, the address of a packed member, dangling local addresses, huge locals, an
  ignored `Result`, an `await` while a mutex is locked), with `-Wall`, `-Wextra`,
  `-Werror` and `-w`.

### Concurrency and errors
- `async`/`await` on any expression and in every control-flow construct, with
  cancellation, `spawn`, `select2`/`join2`, timers and channels over an event loop.
- Threads (`thread_create`/`thread_join`) and atomics (`<atomic>`: 32-bit, 64-bit and
  pointer cells, fetch operations, relaxed forms, fences).
- `try`/`catch`/`finally`/`throw`; an uncaught exception prints its type and value.
- `Result`, `Option` and `Either` with the `?` operator. `Result` with `?` is the way to
  report an expected failure, and `?` passes an error up through functions whose
  `Result` types differ only in the success type; `throw` is for unrecoverable errors
  (spec §6.10).

### C interop and targets
- `extern` functions and variables, with structs and unions passed and returned by value
  in the target's C ABI (AArch64, x86-64 SysV and Windows, 32-bit ARM, 32-bit x86,
  wasm32), callbacks (also ones taking a C `va_list`), `va_list` and variadic functions.
- `<ctypes>`: `size_t`, `ssize_t` and `c_long` for `extern` declarations, passed at the
  target's C width (32 bits on 32-bit targets), so one declaration is right everywhere.
- Inline assembly with outputs, inputs and clobbers; `volatile` accesses; `intrinsic`.
- `#pragma link` and implied libraries, so no `-lm`, `-lc++` or `-lpthread` flags.
- macOS, Linux and Windows hosts; cross-compilation with `--target` (including bare-metal
  `none` triples and `--freestanding`); `-O0` to `-O3`.
- WebAssembly: `wasm32-unknown-emscripten` (linked by `emcc`) and WASI. The standard
  library and async runtime run there (an event loop over `poll()`), and exceptions work
  on emscripten through its runtime.
- Android (`aarch64-linux-android24`, armv7, x86_64; linked with the NDK's clang) and iOS
  (devices and the simulator; linked through `xcrun`).
- Static (`.a`, `.lib`) and shared (`.so`, `.dylib`, `.dll`) library outputs, for apps
  that link Eskiu code from Xcode, Gradle or emscripten builds.
- `--subsystem=windows` links a Windows program that opens its own window without a
  console.

### Standard library
- Collections (`List`, `Map`, `HashMap`, `Set`), strings and bytes, math, sorting, random
  numbers, regex (RE2 syntax over UTF-8), time and calendars, UUIDs.
- `<sort>` is libc++'s `std::sort`, `std::partial_sort` and `std::nth_element`
  (`sort<T>`, `sort_partial<T>`, `sort_nth<T>`), so code ported from C++ orders tied
  elements the same way; `List_reserve`, `List_resize` and `List_copy_from` in `<list>`.
- Files, directories, globbing and temp files (`<fs>`), child processes (`<process>`),
  environment and `.env` files, DNS, sockets, URLs.
- JSON, CSV, base64, hashes (SHA-256, SHA-1, MD5, HMAC-SHA256), multipart forms.
- An HTTP client (`<fetch>`, with HTTPS through the system OpenSSL), and HTTP/1.1, HTTP/2
  and TLS servers, blocking and async.
- Command-line helpers: argument parsing (`<args>`), terminal colors (`<term>`), logging
  (`<log>`) and console input.
- `_result` forms of the calls that report a status code or `null`, so `?` works with the
  standard library: `fetch_result`, `fetch_with_result`, `fs_read_text_result`,
  `fs_write_text_result`, `fs_append_text_result`, `json_parse_result`,
  `process_run_result`, `sh_result`.
- `<libc>` declares the common C library for code that wants it directly.

### Tools
- `eskiuc run file.esk` and `#!/usr/bin/env -S eskiuc run` shebangs.
- `eskiuc repl`, an interactive session over the LLVM JIT (macOS and Linux).
- A package manager: `eskiuc new`, `add`, `build`, `run`, `test` and `fetch`, with git and
  path dependencies pinned in `eskiu.lock`.
- A package can ship the native libraries it binds: `[native.NAME]` in `eskiu.toml` lists a
  prebuilt archive per platform with its SHA-256, which the first build that links the
  library downloads and checks into `~/.eskiu/native`; the build links against it with no
  `-L`.
- `eskiuc fmt`: one statement per line, consistent spacing and indentation (type
  arguments as `List<int>`), without
  changing a program's tokens.
- Editor support for VS Code-based editors and JetBrains IDEs: errors as you type,
  completion, hover, signature help, go to definition, find references, rename, outline,
  formatting and quick fixes.

### Self-hosting
- The compiler is also written in Eskiu (`selfhost/`), built as `eskiuc-esk`, and checked
  against the C++ compiler over the whole test corpus and against itself through a
  three-stage bootstrap. Both compilers give the same diagnostics and warnings.

### Known issues
- `eskiuc repl` is not available on Windows: the JIT does not register the unwind tables
  Windows exceptions need. `eskiuc run` works there.
