# Eskiu 0.13.0

The first public release of Eskiu, a systems language with C's control over memory and
calling conventions. It compiles to native code through LLVM, calls C libraries directly,
and runs a source file like a script with `eskiuc run file.esk`.

From this release on, code that compiles with 0.13.0 keeps compiling in every 0.13.x, and
later changes are announced by warnings first ([stability policy](https://eskiu-lang.org/docs/stability.html)).

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

## What is in it

- **The language.** C syntax and C integer rules, with structs and methods, generics,
  structural interfaces, sum types with exhaustive `match`, closures and trailing blocks
  (`Column(style) { ... }`), operator overloading, `async`/`await`, default parameter values and `dbg(e)`.
- **One way to report errors.** `Result<T, E>` with `?` for expected failures (the
  standard library has `Result` forms of its fallible calls), and exceptions for errors a
  program cannot continue from.
- **Memory with guard rails.** No garbage collector: `alloc`/`free` and explicit
  allocators, `defer`, slices, checked nullable pointers, and opt-in run-time checks
  (`--safe`, or `--safe=bounds,overflow,null` one by one).
- **C interop.** `extern` functions and variables, structs passed by value in each
  target's C ABI, inline assembly, and cross-compilation to ARM and x86 targets and bare
  metal.
- **A standard library for real programs.** Collections, strings, regex, JSON, CSV,
  hashes, files and processes, an HTTP client with HTTPS, and HTTP/1.1, HTTP/2 and TLS
  servers on an async runtime.
- **Scripts.** Statements may sit at the top level of a file, with no `main`, and run in
  order; `#!/usr/bin/env -S eskiuc run` makes the file executable.
- **Tools.** A REPL, a package manager with git and path dependencies, `eskiuc fmt`, and
  editor support for VS Code-based editors and JetBrains IDEs (completion, hover,
  definitions, references, rename, quick fixes).
- **Self-hosted.** The compiler is also written in Eskiu and builds itself.

## Known issues

- `eskiuc repl` is not available on Windows in this release. `eskiuc run` works there.

The full summary is in the [changelog](https://eskiu-lang.org/changelog.html).
