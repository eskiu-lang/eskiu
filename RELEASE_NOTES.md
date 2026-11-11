# Eskiu 0.15.1

Eskiu 0.15.1 is about packages. A build can link the system's copy of a library a package
ships, and falls back to it when the download fails; a package can carry the files a web
build needs next to its page; and `eskiuc build FILE.esk` builds an example or a tool of a
package with the package's dependencies. It also fixes three bugs that the gpu package hit.

## What is new

- **`ESKIU_SYSTEM_LIBS`.** `ESKIU_SYSTEM_LIBS=SDL3 eskiuc build` links the system's SDL3 in
  place of the archive the package names in `[native.SDL3]` (`*` names every library). The
  build looks for `libNAME` in `LIBRARY_PATH`, the directory `pkg-config` gives for it and
  the usual library directories. Both compilers.
- **Builds offline.** When the download of a package's archive fails and the system has the
  library, a build for this machine prints a warning and links the system's copy instead
  of stopping.
- **`[web] assets`.** A package lists the files a browser build needs next to its page:

  ```toml
  [web]
  assets = ["web/gpu-naga.wasm"]
  ```

  A `wasm32-unknown-emscripten` build in the package, or in one that depends on it, copies
  them into the output's directory, such as the translator that gpu 0.6.0's WebGL2 backend
  loads. Older compilers reject the table, so a package that adds it needs 0.15.1.
- **`eskiuc build FILE.esk`** builds that file of the package (an example, a tool) with the
  package's dependencies, to `build/FILE`.
- **`eskiuc doctor` finds SDL3 the way a build does**: the `[native.SDL3]` archive of a
  package, already downloaded or still to download, else the system's `libSDL3`.
- **`install.sh --with-ui`** (`ESKIU_WITH_UI=1` for `install.ps1`) also installs the `ui`
  tool from eskiu-lang/ui's release, checked against its SHA256SUMS.
- **`noundef` and `nounwind`.** The code generator marks scalar parameters and results
  `noundef`, and a function `nounwind` when nothing it calls can throw. Code at `-O2` is
  the same on the test corpus; `-O0` objects are about 4% smaller.

## Fixed

- A program of several input files failed to parse when a later file's imports used a type
  or a generic declared by an earlier file's imports (`Expected ';'` at
  `return Ok<GpuSurface, string>(s);`).
- A generic function instantiated while compiling another function saw that function's
  locals, so `List_free` failed to compile in a function with a local named `free`. Errors
  at a call in the code generator now give the file, line and column.
- Compiling a file of a package directly (`eskiuc src/main.esk`) used a lock that no longer
  matched `eskiu.toml` after a dependency was edited; it now resolves the dependencies again.
- `--target` for the other macOS architecture on a Mac links the program with `cc -target`
  (or `$CC -target`).

Code that compiles with 0.15.0 compiles with 0.15.1. The full list is in the
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
