# Eskiu 0.15.0

Eskiu 0.15.0 lets other languages call Eskiu. A function marked `export` is a C entry
point, and `--emit-header` writes the C header for it, so a C, C++ or Rust program can link
an Eskiu library like any C library. The compiler is also faster (a large app at `-O0` builds
in about half the time), reports its diagnostics as JSON for editors and agents, and gains
`eskiuc doctor`, binaries for Intel Macs, a one-step toolchain on Windows, two opt-in
floating-point and aliasing flags, and faster `<regex>` and `<json>`.

## What is new

- **A faster compiler.** The eskiu-ui kit gallery (about 47,000 lines of IR) builds to an
  object at `-O0` in 0.29 s, against 0.53 s with 0.14.1. The parser no longer tries
  declarations and generic calls by throwing exceptions, a program with no `async` function
  is type-checked once, and AST node checks no longer go through RTTI.
- **`--diagnostics=json`.** Errors and warnings as JSON lines, one object per diagnostic:
  severity, a stable id, the message, the file and range, the `-W` option, hints and notes.
  The ids are listed in `docs/dev/diagnostic-ids.txt`. The text form stays the default.
- **`src_file()`, `src_line()`, `src_col()`.** As a parameter's default value they give the
  caller's position, so a library can record where it was called from:
  `void Text(string s, int line = src_line())`. Elsewhere they are a compile error.
- **Shared libraries offer only their exports.** A `.so`, `.dylib` or `.dll` with `export`
  functions hides every other symbol (on Windows the exports are `dllexport`).
- **`HttpLimits.max_body`** sets the largest request body the HTTP/1.1 and HTTP/2 servers
  accept (1 MiB by default), and **`log_set_rotation(max_bytes, keep)`** rotates a log file by
  size.
- **`export` and `--emit-header`.** `export` before a function definition makes it callable
  from C by its name, with the target's C calling convention, structs and unions by value
  included. Eskiu code calls it as before.

  ```eskiu
  struct Pair { double x; double y; }
  export Pair scale(Pair p, double k) { return Pair{x: p.x * k, y: p.y * k}; }
  ```

  ```bash
  eskiuc lib.esk -o liblib.a --emit-header lib.h
  cc main.c liblib.a -o main        # main.c does #include "lib.h"
  ```

  The header declares the exported functions and every struct, union and enum they reach,
  with bitfields and packing, so a C compiler lays the types out as Eskiu does. It also
  compiles as C++. A parameter or result with no C meaning (a closure, an interface, a
  slice, a sum type, a generic instance) is a compile error that says why. `export` is not
  a keyword: a variable named `export` still works.
- **`eskiuc doctor`.** Lists what the machine has for each kind of build and what to
  install for the rest: the C driver that links programs, git, curl and tar for packages,
  OpenSSL for https in `<fetch>`, SDL3 for the gpu and audio packages, and the toolchains of
  the cross targets (Emscripten, the Android NDK, the iOS SDK, devkitARM).
- **Intel Macs.** The release now has `eskiuc-macos-x86_64.tar.gz`, and `install.sh` picks
  it on an Intel Mac.
- **Windows in one step.** When no C compiler is on PATH, `install.ps1` also installs
  llvm-mingw (checked against a pinned SHA-256) next to `eskiuc`, which finds it there. Set
  `ESKIU_TOOLCHAIN=1` to install it anyway, or `0` to skip it.
- **`-ffp-contract=fast` and `-fstrict-aliasing` (opt-in).** The first lets the optimizer
  fuse a multiply and an add into one instruction; the second gives it C's rule that a
  store of one scalar type does not change a value of another. Both are off by default, so
  results stay the same on every target and type punning through pointers keeps working.
- **Faster `<regex>`.** A search finds where a match ends and starts with lazily built
  DFAs, and only runs the slower matcher over the match itself, for its groups. Times on the
  LangArena benchmarks, same machine:

  | | 0.14.1 | 0.15.0 |
  |---|---|---|
  | Log parsing | 1.79 s | 1.19 s |
  | Template matching (`{{(.*?)}}`) | 2.63 s | 2.31 s |

- **`json_parse_doc`.** Parses into a read-only `JsonDoc` whose nodes and strings live in
  one arena: about three times faster than `json_parse` followed by `JsonValue_free`.
- **`eskiuc fmt --width N`** breaks code lines longer than N after the commas of their
  outermost bracketed list. Without `--width` the formatter's output does not change.
- **Safer TLS defaults.** `<tls>` servers and the HTTPS client of `<fetch>` ask for TLS 1.2
  or newer and turn off compression and renegotiation, whatever the OpenSSL defaults are. A
  server whose key does not match its certificate is refused when it starts.
- **The self-hosted compiler** links packages' native libraries and can build programs
  through its own C backend (`eskiuc-esk --backend=eir`) with any C compiler, without clang
  or LLVM.

Fixed: an import of `C:/...` or `C:\...` is absolute on Windows and a path with backslashes
keeps its directory; a `?:` with a `null` arm inside a generic function compiles; several
fixes in the EIR backends and `<fetch>`.

Code that compiles with 0.14.1 compiles with 0.15.0. The full list is in the
[changelog](https://eskiu-lang.org/changelog.html).

## Install

**macOS (Apple Silicon and Intel) and Linux (x86-64, arm64)**

```bash
curl -fsSL https://eskiu-lang.org/install.sh | sh
eskiuc doctor
```

**Windows (x86-64)**

```powershell
powershell -c "irm https://eskiu-lang.org/install.ps1 | iex"
eskiuc doctor
```

Or download an archive below. To build from source you need LLVM 21 or newer (LLVM 22
recommended, built with RTTI) and CMake 3.20+:

```bash
git clone https://github.com/eskiu-lang/eskiu
cd eskiu && cmake -S . -B build && cmake --build build
```

## Known issues

- `eskiuc repl` is not available on Windows.
