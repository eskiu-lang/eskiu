# Changelog

All notable changes to Eskiu are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
project follows the compatibility rules in [docs/lang/stability.md](docs/lang/stability.md).

---

## [Unreleased]

### Added
- `-ffp-contract=fast` lets the compiler fuse a multiply and an add into one fused
  multiply-add (rounded once); without it each floating-point operation rounds, so results
  stay the same on every target. Both compilers.
- `-fstrict-aliasing` gives the optimizer C's aliasing rule (scalar type-based alias
  information): a store of one scalar type is assumed not to change a value of another.
  Unions, bytes and a variable read through a pointer cast keep aliasing everything. Off by
  default; both compilers.
- `eskiuc fmt --width N` also breaks code lines longer than N after the commas of their
  outermost bracketed list; without it the formatter's output does not change.
- `<json>`: `json_parse_doc(src)` parses into a `JsonDoc` whose nodes and strings live in
  one arena, about three times faster than `json_parse` plus `JsonValue_free`; the document
  is read-only and `JsonDoc_free` frees it.
- The self-hosted compiler links a package's `[native.NAME]` libraries, fetching and
  checking them as the C++ compiler does.
- `tests/index.sh` prints the test catalog from each test's first comment, and `run.sh`
  fails on a test without one.

### Changed
- `<regex>` searches with a lazily built DFA for where the match ends and one over the
  reversed pattern for where it starts, and runs the Pike VM only over the match, for its
  groups: two to three times faster on log-like text, with the same results (checked
  against the Pike VM alone by a test and the regex fuzz target).
- TLS (`<tls>` servers and `<fetch>`'s HTTPS client) asks for TLS 1.2 or newer and turns
  off compression and renegotiation, whatever the OpenSSL defaults; a server whose key
  does not match its certificate is refused when the context is made. The client loads
  OpenSSL and makes its shared contexts once, also from several threads at once.
- The kernel demo's heap is a first-fit allocator, so `free` gives memory back.
- `--backend=eir` takes `-O` where it writes C (any target but arm64) and compiles the C
  at that level, always with `-fno-strict-aliasing`; on the ARM64 path `-O` stays an error.
- Building against an LLVM without RTTI (the llvm-project release tarballs) stops at
  configure time with an explanation instead of failing to link. LLVM's headers are
  included as system headers, and the compiler's own sources build without warnings (CI
  builds with `-DESKIU_WERROR=ON`).
- `fetch_basic` and `fetch_async` use numeric addresses only; the host name cases moved to
  `fetch_dns`, which `ESKIU_RESOLVER_TESTS=0` skips with `dns_connect`.
- CI runs the Windows build on pull requests and on `main`, the EIR C backend on Linux,
  and fails when the editor modes exceed their time budget; a release fails when its
  Windows build does.

---

## [0.14.1] - 2026-10-29

### Added
- Native EIR backends that skip LLVM's code generator: `--emit-asm` (ARM64), `--emit-c`
  (C99), `--emit-obj`, and `--backend=eir`, which builds through the normal driver
  (`#pragma link`, implied libraries, `-c`, `.a`, `eskiuc run`). Exceptions, threads and
  `exception_text` run on a small C runtime. Options that only change LLVM's output (`-O`,
  `--safe`, `--freestanding`, ...) are errors under it. See `docs/dev/eir-backends.md`.
- `tests/eir/check.sh` runs the run tests through a backend (as `tests/run.sh` does, C
  companions included) and fails when a test that passed stops passing; CI runs it on macOS.
- The self-hosted compiler's `--test-eir` is a port of the C++ EIR builder and prints the
  same text for every run test; `tests/selfhost/eir_parity.sh` gates it in CI.
- The EIR backends pass structs and unions by value to and from C functions and hand C a
  function taking or returning one (or a `va_list`) through a thunk with the C signature.
- `<csv>`: `CsvReader` reads a CSV one row at a time with no String per field
  (`CsvReader_init`, `CsvReader_next`, `CsvReader_field`, ...): each row's fields are
  NUL-terminated in a buffer the reader reuses. `csv_parse` is now built on it, so both
  read every input the same way (checked against the previous `csv_parse` over 300000
  random inputs). Summing three columns of a 100000-row CSV takes half the time it does
  through `csv_parse`.
- `String_append_bytes(s, p, n)` (`s.append_bytes(p, n)`) appends `n` bytes from a
  pointer, NULs included; it was only available as an internal function.
- `<regex>`: `Regex_search_at(&re, text, len, start, &m)` searches `len` bytes from offset
  `start`, and `Regex_iter` / `RegexIter_next` go through every match as Go's `FindAll`
  does (after an empty match the search moves on one code point).

### Fixed
- A field of a packed struct (`packed`, `#pragma pack(N)`, or a struct inside one) was read
  and written with its type's alignment although its address may not have it, which LLVM
  may turn into wider or paired accesses (and which traps on 32-bit ARM). Each such access
  now carries the alignment its address is known to have (1 under `packed`, at most N under
  `pack(N)`), in both compilers.
- `-O2` and `-O3` now run LLVM's loop and SLP vectorizers, as clang does; the pipeline had
  them off. CLBG n-body runs in about half the time.
- A C math `extern` with the C signature (`double sqrt(double)`, `float powf(float,
  float)`, ...) is declared as reading and writing no memory, so the optimizer can hoist,
  merge and inline it (`sqrt` becomes an instruction): `fabs`/`floor`/`fmin`/... on every
  target, the functions that may set `errno` only on Darwin, where clang defaults to
  `-fno-math-errno` and libm never sets it. Not under `--freestanding`.
- `<json>`: `JsonValue_push` walked the whole array on each call, so building an array of
  n elements took O(n^2) time (80000 elements: 4.3 s, now under 0.1 s). `JsonValue_len` and a
  loop over `JsonValue_at(v, i)` are linear too.
- `<json>` stringifies numbers with its own shortest round-trip algorithm (Ryu) instead of
  `printf` and `strtod`, and copies plain strings in place: building and stringifying a
  document is about 2.5 times faster (LangArena Json::Generate 2.2 s to 0.9 s). The text is
  byte for byte the same as before (checked over 82 million values).
- `json_parse` reads a number of up to 15 significant digits with a decimal exponent within
  10^±22 straight from the text with one exact multiplication or division (the double
  `strtod` gives), and the rest with `strtod` in place, instead of copying each number
  into a String for `atof`; plain runs inside strings are copied at once. Parsing a
  document of numbers is about 1.4 times faster.
- `<map>`: `Map<V>` keeps each key's hash, so a lookup compares hashes before strings and
  a removal or a growth no longer rehashes the keys it moves; lookups and removals are up
  to twice as fast. The iteration order is unchanged.
- `<base64>` encodes about 3.6 times and decodes about 4.7 times faster (500 KB buffers:
  encode 1.53 s to 0.42 s, decode 1.50 s to 0.32 s, ahead of Go's `encoding/base64`). Each
  character is looked up in tables that give its bits already in place, and word-aligned
  buffers are read and written a word at a time. Results and errors are the same as before
  for every input.
- EIR: a cast between `float` and `double`, or between a pointer and an integer, now emits
  its conversion; a closure call converts its arguments to the parameter types; a top-level
  function used as a closure goes through a `__fnptr_` thunk; an extern's function
  parameter receives a C function pointer; `continue` inside a `switch` no longer crashes
  the compiler; a repeated prototype no longer defines the function twice.
- `--test-eir` output is deterministic: records are laid out in declaration order and as
  the program first names them (no longer every generic instance the checker saw, in hash
  order), and a vtable global prints its functions instead of an uninitialized operand.
  The async lowering names its temporaries in the same order whichever C++ compiler built
  `eskiuc`.
- EVM: integer `/` and `%` by zero stop the program instead of yielding 0, and
  `ptrtoint`, `inttoptr`, `fptrunc` and `fpext` are executed.
- EIR integers follow C everywhere: the builder makes the promotions and usual arithmetic
  conversions explicit and every backend holds a value normalized to its type; constant
  expressions fold with the type checker's rules.
- EIR lowering now covers `alloc_with`, struct and union layout as C lays it out (bitfields,
  unnamed bitfields, `#pragma pack`), generic enum instances, field defaults, `for-in`,
  labeled `break`/`continue`, `defer` scoping, `errdefer` on `?`, `dbg`, operators and
  locals inside generic instances, and evaluates an assignment's target once and first.
- `<regex>` had no way to search from an offset: searching `text + off` ran `strlen` on
  every call, so finding all matches was quadratic, and it lost the text before the cut, so
  `^`, `\b` and `\B` were wrong there. `Regex_search_at` keeps that text as context and
  reports offsets into the whole text.
- `<regex>` searches are much faster: the search skips text where no match can start (by
  the bytes a match can begin with, and by a byte every match must contain), its scratch
  memory is allocated once per compiled regex instead of on every call (a search running
  while another thread holds it allocates its own, so a `Regex` can still be searched from
  several threads at once), and threads that cannot read the next byte are dropped. A
  log-scanning benchmark went from 25.2 s to 1.8 s and a template one from 7.4 s to 3.1 s.

## [0.14.0]

### Added
- `catch (_)` catches any exception without binding a variable; it is the last catch of a
  `try`. `exception_text()`, inside any catch, describes what was caught as the
  uncaught-exception report does (`int 7`, `string "boom"`). The text belongs to its catch
  block, also across an `await` in an `async` function, and a function of the program's own
  named `exception_text` hides the builtin.
- `[native.NAME]` packages for iOS (`ios-arm64`, `ios-arm64-simulator`,
  `ios-x86_64-simulator`) and Android (`android-arm64`, `android-x86_64`), and for any
  `--target` build: the archive for the target's platform is fetched and linked as for the
  host.

### Fixed
- `--target x86_64-w64-mingw32` passed structs to C functions with the System V convention
  instead of the Windows x64 one, so a call into a DLL could crash. Every `--target` is now
  normalized first (`x86_64-w64-windows-gnu`).
- A build for iOS or Android no longer embeds an rpath to this machine's package cache.

## [0.13.0]

The first public release. Code that compiles with 0.13.0 keeps compiling in every 0.13.x
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
- Trailing blocks: a call statement may end in a block, passed as its last argument, so
  `Column(style) { ... }` is `Column(style, void() { ... })`.
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
