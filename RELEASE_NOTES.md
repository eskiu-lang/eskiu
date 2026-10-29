# Eskiu 0.14.1

Eskiu 0.14.1 makes optimized code and the standard library much faster, fixes how fields
of packed structs are accessed, and adds native EIR backends behind a flag. We measured it
by porting all 50 benchmarks of [LangArena](https://github.com/kostya/LangArena) to Eskiu:
on the 39 algorithmic ones it now runs as fast as C, and the C++ and the self-hosted
compiler give the same output on all 50.

## What is new

- **Faster `-O2` and `-O3`.** The optimizer now runs LLVM's loop and SLP vectorizers, as
  clang does; they were off. CLBG n-body takes half the time it did.
- **C math functions inline.** An `extern` such as `double sqrt(double)` is declared as not
  touching memory, so the optimizer can move, merge and inline it (`sqrt` becomes one
  instruction). The ones that may set `errno` get this only on Apple platforms, where
  clang does the same and libm never sets it.
- **Fixed: fields of packed structs.** A field of a `packed` struct, a `#pragma pack(N)`
  struct or a struct inside one was read and written with its type's alignment, which its
  address may not have. LLVM could turn that into wider or paired accesses, and on 32-bit
  ARM it can trap. Each such access now carries the alignment its address is known to
  have.
- **A faster standard library.** Times for the LangArena benchmarks that use it:

  | | 0.14.0 | 0.14.1 |
  |---|---|---|
  | Log parsing (`<regex>`) | 26.3 s | 1.8 s |
  | Template matching (`<regex>`) | 7.7 s | 2.7 s |
  | JSON building and output (`<json>`) | 2.5 s | 0.85 s |
  | Base64 encode / decode | 1.54 / 1.44 s | 0.42 / 0.32 s |
  | CSV summing (`<csv>`, with `CsvReader`) | 1.32 s | 0.65 s |
  | A string-keyed cache (`<map>`) | 1.80 s | 1.02 s |

  `JsonValue_push` no longer walks the whole array, so building a large array is linear
  instead of quadratic. JSON numbers are written by a shortest round-trip algorithm and
  read without a copy; the text is the same as before. `Map<V>` keeps each key's hash and
  iterates in the same order.
- **New functions.** `Regex_search_at` searches from an offset and keeps the text before
  it as context for `^` and `\b`, and `Regex_iter` walks every match. A compiled `Regex`
  can still be searched from several threads at once. `CsvReader` reads a CSV one row at
  a time without allocating per field. `String_append_bytes` appends bytes from a pointer.
- **Native EIR backends (opt-in).** `--backend=eir` builds through Eskiu's own IR without
  LLVM's code generator: ARM64 assembly on arm64, C99 elsewhere (`--emit-asm`,
  `--emit-c`). The default backend is still LLVM.

Code that compiles with 0.14.0 compiles with 0.14.1: the new functions and flags only add
to what was there. The full list is in the [changelog](https://eskiu-lang.org/changelog.html).

## Install

**macOS (Apple Silicon) and Linux (x86-64, arm64)**

```bash
curl -fsSL https://eskiu-lang.org/install.sh | sh
eskiuc --version
```

Or download a tarball below and unpack it into a prefix (`tar -xzf eskiuc-<platform>.tar.gz -C /usr/local`).

**Windows (x86-64)**

```powershell
Expand-Archive eskiuc-windows-x86_64.zip -DestinationPath C:\eskiu   # then add C:\eskiu\bin to PATH
eskiuc --version
```

Or build from source (LLVM 21 or newer, LLVM 22 recommended, and CMake 3.20+):

```bash
git clone https://github.com/eskiu-lang/eskiu
cd eskiu && cmake -S . -B build && cmake --build build
```

## Known issues

- `eskiuc repl` is not available on Windows in this release. `eskiuc run` works there.

The full summary is in the [changelog](https://eskiu-lang.org/changelog.html).
