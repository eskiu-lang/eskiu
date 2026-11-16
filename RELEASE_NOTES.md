# Eskiu 0.16.0

Eskiu 0.16.0 adds Apple Vision Pro as a target, upload progress to `<fetch>`, and type-name
navigation in the editors.

## What is new

- **visionOS.** `--target arm64-apple-xros1.0` (and `arm64-apple-xros1.0-simulator`) builds
  for Apple Vision Pro: it links through `xcrun -sdk xros` / `xrsimulator` with no `$CC`, and
  predefines `TARGET_OS_IPHONE` and `TARGET_OS_VISION`. Both compilers
  ([cross-compile.md](https://eskiu-lang.org/docs/cross-compile.html)).
- **Upload progress.** `FetchOptions.body_sent`, when set, points at a counter the request
  updates with the body's bytes sent so far, as they go.
- **Type names in the editors.** Go to definition, find references, rename and hover work on
  a type name wherever one is written (parameters, results, locals, fields, `*T`, `?*T`,
  `T[]`, generic arguments, casts, `sizeof(T)`, struct literals, constraints, aliases) and
  at its declaration. Hover shows the declaration.

## Fixed

- Editors (VS Code, IntelliJ, any LSP client): a file that is a part of a module
  (`import "part.esk";`) is checked in its module, so diagnostics, hover, navigation and
  completion see the types and names the module declares before it.

## Changed

- The compiler's sources, in C++ and in Eskiu, are split into files of about 600 lines by
  topic, and the stdlib's larger modules keep their parts in private `_module/` directories
  that tools skip. Programs compile and behave the same.

Code that compiles with 0.15.1 compiles with 0.16.0. The full list is in the
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
