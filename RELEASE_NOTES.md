# Eskiu 0.16.3

Eskiu 0.16.3 is a security release. It fixes the findings of an audit of 0.16.2: memory
safety and limits in the standard library, symbolic links and lock records in the package
manager, compile-time blowups in both compilers, and the playground, editor and CI surfaces.
Update if you build packages you did not write, serve HTTP with the standard library, or run
the playground.

## Security

- **Standard library.**
  - `<eventloop>` on Windows and WebAssembly grows its poll set instead of writing past a
    fixed table (4096 or 1024 descriptors).
  - `fetch_last_error()` keeps one message per thread; two threads failing at once could
    corrupt the heap.
  - The HTTP/1.1 servers and `http_recv` hold a request head to 64 KiB however it is split.
  - `fs_temp_file` and `fs_temp_dir` reject a prefix with a path separator.
- **Package manager.**
  - A web build copies a package's `[web]` assets only from inside the package: symbolic
    links out of it are skipped.
  - A native archive whose `lib` directory is a symbolic link, or holds one leading out of
    the archive, is refused.
  - `eskiu.toml` rejects control characters in a dependency's `git`, `rev` or `path`, which
    could add records to `eskiu.lock`.
- **Compilers.**
  - Macro expansion stops at 16 MiB of replacement text per file with an error.
  - A generic async function with many type parameters no longer builds every subset of
    them. Both compilers.
- **Editors.** The VS Code extension honors Workspace Trust, and neither it, the language
  server nor the IntelliJ plugin runs a compiler from a repository you have not trusted.
- **Playground.** Output is capped as it is read, a program's whole process tree is killed
  when it ends, and programs run with no network.
- **CI.** Read-only tokens except where a release is made, actions pinned by commit, store
  tokens only in the steps that publish.

`fs_temp_file` and `fs_temp_dir` now fail (EINVAL) for a prefix containing `/` or `\`; other
code that compiles with 0.16.2 compiles and behaves the same. The full list is in the
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
