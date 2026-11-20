# Eskiu 0.16.2

Eskiu 0.16.2 passes large structs between functions by pointer, which makes programs that pass
them by value smaller and faster, most of all on WebAssembly.

## Changed

- **Large structs pass by pointer.** A struct, union or array parameter over 16 bytes now
  passes between Eskiu functions as a pointer to a copy the caller makes, on every target and
  call path (direct, method, generic, interface, function value, closure, async). LLVM used to
  spread such a struct into one argument per field at each call. A call taking a large struct
  literal shrinks to about a fifth: an element of the ui package (`Row(Style{padding: 8}) {}`)
  costs about 105 bytes of WebAssembly instead of 468, and a ui app's module is 16% smaller.
  A struct literal also starts as a copy of its type's constant defaults and stores only the
  fields it gives. By-value semantics are unchanged: a function that changes its parameter
  never changes the caller's value. `extern` C functions, `export` entries and C callbacks
  keep the C ABI.
- **Rebuild everything.** Objects built by 0.16.1 do not link with objects built by 0.16.2.

## Fixed

- **Self-hosted compiler.** "cannot assign to read-only location" names the location, as
  eskiuc does.

Code that compiles with 0.16.1 compiles with 0.16.2. The full list is in the
[CHANGELOG](https://github.com/eskiu-lang/eskiu/blob/main/CHANGELOG.md).

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
