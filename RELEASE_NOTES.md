# Eskiu 0.17.0

Eskiu 0.17.0 makes the edit, build and test cycle of a large program shorter and clearer.
A game of 20,000 lines ran its test suite in 21 minutes with 0.16.3 and in under 3 with
0.17.0, and a rebuild that changes nothing is now close to instant.

## Faster builds

- **Large records are copied in memory.** Assigning a struct literal to a big record
  (`*self = State{}`) or copying one made LLVM build one value per field, and its
  instruction selection then took up to a minute for a single file at -O2. Such copies
  are now a `memcpy`/`memset`, as Clang does: the game's slowest test went from 67.5 s to
  2.4 s.
- **An object cache, on by default.** A compile reuses the object of an earlier one with
  the same sources (every imported file, the standard library and packages included),
  compiler, target and flags; only parsing and the link run again, and the warnings are
  printed again. Objects live in `~/.eskiu/cache/obj`, at most 1 GiB. `--no-cache` or
  `ESKIU_NO_CACHE=1` turn it off.
- **`--time`** prints the time of each compile phase, the link and the total.

## Testing

- **`eskiuc test`** runs several tests at a time (`-j N`), selects them by name or glob,
  stops a test after `--timeout` seconds and tells a failed build, a failed test, a
  timeout and a crash apart. `--json` prints one line per test, for CI and tools.

## Clearer errors

- **A failed `--safe` check says what and where:** `main.esk:12:20: index 7 out of bounds
  for length 4`, then the program aborts. `--dev` turns on every check and `-Wall`.
- **A name defined twice** points at the first definition and the module or package it is
  in, without the cascade of errors that used to follow inside the other module.
- **A syntax error from a macro** names the macro: `'CLOCK_MONO' is a macro (from <time>)
  and expands to '6' here`.
- A keyword used as a name gets one clear message wherever it appears.

## Editors

- VS Code and the language server (Neovim, Helix, Zed, the JetBrains plugin) now share
  one implementation and check a file in the program it belongs to (the package's `main`),
  so a file that uses what an earlier import declared no longer shows false errors.
  Unsaved buffers are checked with the new `--overlay`. Find references and rename cover
  the whole program.

## Changed

- **Exit codes.** A wrong command line exits with 2 in every command (an unknown option,
  a bad value), 1 is a failed compile, link or test, 0 success. A failed `--safe` check
  now ends the program with SIGABRT (exit status 134) instead of SIGTRAP (133).
- `<args>`: an option registered without dashes (`"w"`, `"width"`) now matches `-w` and
  `--width`; it used to never match.
- The browser playground is gone from the site; the quickstart installs the compiler in a
  minute, and agents run code through the MCP.

Code that compiles with 0.16.3 compiles with 0.17.0. Scripts that check the exit status of
a `--safe` failure or of a usage error need the new codes. The full list is in the
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
