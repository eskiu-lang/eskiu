# Eskiu 0.16.1

Eskiu 0.16.1 fixes HTTPS on Windows with MSYS2's OpenSSL 4 and WebAssembly builds with newer
emsdks, says which toolchain is missing for a cross build, and adds the star-first spelling
of a const pointer and `-Wformat` checks for printf-style functions a program declares.

## Fixed

- **HTTPS with OpenSSL 4.** `<fetch>` also loads OpenSSL 4 (`libssl-4-x64.dll`,
  `libssl.so.4`, `libssl.4.dylib`, Homebrew's `openssl@4`). MSYS2 ships OpenSSL 4.1, so an
  `https://` request on Windows failed with `FETCH_ERR_NO_TLS`.
- **WebAssembly with exceptions.** Newer emsdks no longer link in C++ mode by default when
  the driver is emcc, so a program that throws or catches had `__cxa_begin_catch` and
  `__cxa_end_catch` undefined. eskiuc now passes `-sDEFAULT_TO_CXX=1` when the program uses
  exceptions.
- **A missing cross toolchain is named.** A build for WebAssembly, Android, iOS or visionOS
  without emcc, the NDK or xcrun says which one is missing and how to get it, instead of
  falling back to the host's cc and its "unrecognized option" errors.
- **Self-hosted compiler.** A member access through a const pointer emitted invalid IR, and
  reassigning a local `int* const` was not an error.

## Added

- **`*T const`.** A const pointer can be written star first, `*T const` (and
  `const *T const`), the same type as `T* const`. The docs and the Book now write every
  pointer star first: `*T`, `const *T`, `*T const`.
- **`-Wformat` for your own printf-style functions.** A variadic function or extern whose
  last fixed parameter is `string fmt` has its calls checked like printf's.

Code that compiles with 0.16.0 compiles with 0.16.1. The full list is in the
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
