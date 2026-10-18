# Changelog

All notable changes to Eskiu are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
project follows the compatibility rules in [docs/lang/stability.md](docs/lang/stability.md).

---

## [0.12.0] - 2026-10-17

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
- Default parameter values (`void connect(string host, int port = 443)`) and `dbg(e)`,
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
  that traps on out-of-bounds indexes, signed overflow and null dereferences.
- Locals, `alloc_with` blocks and sum-type values start at zero.
- Warnings on by default for common C mistakes (string comparison by address, printf
  formats, the address of a packed member, dangling local addresses, huge locals), with
  `-Wall`, `-Wextra`, `-Werror` and `-w`.

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
  in the target's C ABI (AArch64, x86-64 SysV and Windows, 32-bit ARM, 32-bit x86),
  callbacks, `va_list` and variadic functions.
- Inline assembly with outputs, inputs and clobbers; `volatile` accesses; `intrinsic`.
- `#pragma link` and implied libraries, so no `-lm`, `-lc++` or `-lpthread` flags.
- macOS, Linux and Windows hosts; cross-compilation with `--target` (including bare-metal
  `none` triples and `--freestanding`); `-O0` to `-O3`.

### Standard library
- Collections (`List`, `Map`, `HashMap`, `Set`), strings and bytes, math, sorting, random
  numbers, regex (RE2 syntax over UTF-8), time and calendars, UUIDs.
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
- `eskiuc fmt`: one statement per line, consistent spacing and indentation, without
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
