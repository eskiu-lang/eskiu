# Building Eskiu from Source

## Requirements

| Dependency   | Minimum                                | Tested      |
| ------------ | -------------------------------------- | ----------- |
| LLVM         | 21                                     | 22.x        |
| CMake        | 3.20                                   | 3.x         |
| C++ compiler | C++17 (clang++ recommended, g++ works) | clang++ 17+ |
| git          | any                                    | n/a         |

LLVM is the only non-trivial dependency. It must be built with RTTI (`LLVM_ENABLE_RTTI=ON`), as the packaged LLVMs below are (apt.llvm.org, Homebrew, Alpine, MSYS2). The release tarballs on the llvm-project GitHub page are built without RTTI and cannot be used: configuring against one stops with an error (see [Troubleshooting](#llvm-built-without-rtti)). The compiler links the `support`, `core`, `irreader` and native code generation components, plus the AArch64, X86, ARM and WebAssembly backends used for cross-compilation.

To use a prebuilt compiler instead, see the install instructions in the [README](../../README.md#install). A built `eskiuc` calls the system C compiler (`cc`, `clang` or `gcc`) to link executables, so one must be installed.

---

## macOS

### Install dependencies

```bash
brew install llvm cmake
```

Homebrew installs LLVM into a versioned prefix (e.g. `/opt/homebrew/opt/llvm`) and does not add it to `PATH` by default. Export the necessary paths before building:

```bash
export PATH="$(brew --prefix llvm)/bin:$PATH"
export LLVM_DIR="$(brew --prefix llvm)/lib/cmake/llvm"
```

Add these to your shell profile (`~/.zshrc` or `~/.bash_profile`) to make them permanent.

Homebrew's `llvm` formula tracks the newest release (LLVM 23 at the time of writing). Eskiu builds against it, but CI and the release binaries use LLVM 22. To match them exactly, install `llvm@22` and use `$(brew --prefix llvm@22)` in the two lines above.

### Build

```bash
git clone https://github.com/eskiu-lang/eskiu.git
cd eskiu
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -- -j$(sysctl -n hw.logicalcpu)
```

Expected output (last few lines):

```
[ 97%] Linking CXX executable eskiuc
[ 97%] Built target eskiuc
[100%] Building the Eskiu-written compiler (eskiuc-esk) with the C++ seed
[100%] Built target eskiuc-selfhost
```

The compiler binary is at `build/eskiuc`. When clang is available, the build also produces `build/eskiuc-esk`, the same compiler written in Eskiu and compiled by `build/eskiuc`. Pass `--target eskiuc` to `cmake --build` to build only the C++ compiler.

---

## Linux (Ubuntu / Debian)

### Install dependencies

```bash
sudo apt-get update
sudo apt-get install -y llvm-22-dev clang-22 cmake git
```

For Ubuntu 22.04 or older, the LLVM 22 packages are in the LLVM apt repository:

```bash
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 22
sudo apt-get install -y llvm-22-dev
```

### Build

```bash
git clone https://github.com/eskiu-lang/eskiu.git
cd eskiu
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR=/usr/lib/llvm-22/lib/cmake/llvm \
  -DCMAKE_CXX_COMPILER=clang++-22
cmake --build build -- -j$(nproc)
```

---

## Linux (Alpine)

### Install dependencies

```bash
apk add llvm22-dev cmake clang22 git make
```

### Build

```bash
git clone https://github.com/eskiu-lang/eskiu.git
cd eskiu
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR=/usr/lib/llvm22/lib/cmake/llvm \
  -DCMAKE_CXX_COMPILER=clang++
cmake --build build -- -j$(nproc)
```

---

## Windows (MSYS2)

Windows builds use the MSYS2 MINGW64 environment. From a MINGW64 shell:

```bash
pacman -S --needed git mingw-w64-x86_64-toolchain mingw-w64-x86_64-llvm \
  mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja
git clone https://github.com/eskiu-lang/eskiu.git
cd eskiu
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR="$MINGW_PREFIX/lib/cmake/llvm" \
  -DCMAKE_CXX_COMPILER=g++
cmake --build build --target eskiuc
```

The result is `build/eskiuc.exe`. It links executables through the MinGW `gcc`.

---

## Build Options

### Debug vs Release

```bash
# Release: optimized binary, suitable for benchmarks
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Debug: unoptimized, assertions enabled, better stack traces
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
```

Without `-DCMAKE_BUILD_TYPE` the build is Release: a compiler built with no optimization
compiles programs about three times slower.

### Incremental builds

After changing source files, just re-run the build step; CMake tracks dependencies:

```bash
cmake --build build -- -j$(nproc)
```

### Warnings as errors

The compiler's own sources build without warnings under `-Wall -Wextra -Wpedantic` (LLVM's
headers are included as system headers, so their warnings are not reported). CI keeps it
that way with `-DESKIU_WERROR=ON`, which adds `-Werror` (`/WX` with MSVC):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DESKIU_WERROR=ON
```

### Clean rebuild

Delete the build directory and start over:

```bash
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -- -j$(nproc)
```

---

## Verify the Build

### Version check

```bash
./build/eskiuc --version
```

Expected output:

```
Eskiu 0.17.0 (LLVM 22.x.x)
```

The LLVM version will reflect whichever version is installed on the host.

### Smoke test

```bash
./build/eskiuc examples/hello.esk --test-codegen
```

Expected output (LLVM IR for `hello.esk`):

```llvm
Generating LLVM IR: examples/hello.esk
========================================================
; ModuleID = 'eskiu'
source_filename = "eskiu"
target datalayout = "..."
target triple = "arm64-apple-darwin..."

@0 = private unnamed_addr constant [19 x i8] c"Hello from Eskiu!\0A\00", align 1
@1 = private unnamed_addr constant [12 x i8] c"Result: %d\0A\00", align 1

declare i32 @printf(ptr, ...)

define i32 @add(i32 %a, i32 %b) {
entry:
  ...
}

define i32 @main() {
entry:
  ...
}
```

The target lines and the exact IR body vary with the host and the LLVM version, but the module must contain `@printf`, `@add`, and `@main`.

---

## Troubleshooting

### LLVM not found

```
CMake Error: Could not find a package configuration file provided by "LLVM"
```

Point CMake at the correct LLVM cmake directory:

```bash
cmake -S . -B build -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
```

Common paths:

| Platform               | Path                                    |
| ---------------------- | --------------------------------------- |
| macOS (Homebrew arm64) | `/opt/homebrew/opt/llvm/lib/cmake/llvm` |
| macOS (Homebrew x86)   | `/usr/local/opt/llvm/lib/cmake/llvm`    |
| Ubuntu apt llvm-22     | `/usr/lib/llvm-22/lib/cmake/llvm`       |
| Alpine apk llvm22      | `/usr/lib/llvm22/lib/cmake/llvm`        |

You can also run `llvm-config --cmakedir` (substituting the versioned binary name if needed) to get the correct path:

```bash
llvm-config-22 --cmakedir
```

### C++17 errors

If the system default compiler does not support C++17, specify the compiler explicitly:

```bash
cmake -S . -B build \
  -DCMAKE_CXX_COMPILER=clang++-22 \
  -DCMAKE_C_COMPILER=clang-22
```

### Linker errors: undefined LLVM symbols

If you see undefined symbols from LLVM:

1. Confirm the installed LLVM version matches the headers used at configure time (`llvm-config --version`).
2. On Ubuntu, ensure you installed `llvm-22-dev` (not just `llvm-22`); the `-dev` package contains the static libraries.
3. On Alpine, `llvm22-dev` is the correct package name; `llvm22` alone does not ship the `.a` archives.

### LLVM built without RTTI

```
CMake Error: LLVM at ... was built without RTTI (LLVM_ENABLE_RTTI=OFF), which eskiuc needs.
```

The compiler uses `dynamic_cast` and derives from LLVM classes (`llvm::cl::opt`), so it
needs the type information an RTTI build of LLVM exports. The release tarballs on the
llvm-project GitHub page (`clang+llvm-22.x-*.tar.xz`) are built without it; linking against
one would fail with `undefined symbol: typeinfo for llvm::cl::GenericOptionValue`, so the
configure step stops first. Use a packaged LLVM instead (apt.llvm.org `llvm-22-dev`,
Homebrew `llvm@22`, Alpine `llvm22-dev`, MSYS2 `mingw-w64-x86_64-llvm`) and point
`LLVM_DIR` at it.

---

## Command Line

`eskiuc --help` lists the compiler's options; `eskiuc COMMAND --help` (or `-h`) prints the
usage of a command (`new`, `add`, `build`, `run`, `test`, `fetch`, `fmt`, `repl`,
`doctor`) and does nothing else. For `run`, only a `--help` before the script is the
command's: one after it is the program's argument.

### Timing a build

`--time` prints the wall time of each phase of a compile on stderr: loading and parsing,
type checking, IR generation, optimization, writing the object, linking, and the total.
`ESKIU_TIME_PHASES=1` in the environment does the same for every compile (a Makefile's,
say). See [compile speed](../dev/compile-speed.md) for what each phase is.

```
$ eskiuc --time -O2 app.esk -o app
phase start                 0.0 ms  (total      0.0 ms)
phase load+parse           21.4 ms  (total     21.4 ms)
...
phase emit-object        1830.2 ms  (total   1985.0 ms)
phase link                 40.1 ms  (total   2025.1 ms)
phase total              2025.3 ms
```

### Object cache

By default the compiler keeps the object of each compile that writes an executable or an object file,
and a later compile of the same sources with the same compiler, target and flags reuses
it: only loading, parsing and the link run again. A test suite built one file at a time
gets the most from it, since most of its files do not change between runs. An edit to any
file the program imports (the standard library and packages included), another flag,
another target or another build of the compiler makes a new object. The warnings of the
compile are printed again on a reuse.

| Variable or flag | Effect |
|---|---|
| (default), `--cache` | Use the cache. |
| `--no-cache`, `ESKIU_NO_CACHE=1`, `ESKIU_CACHE=0` | Do not use it (the variables reach every compile of a Makefile). |
| `ESKIU_CACHE_DIR=DIR` | Keep the objects in `DIR` instead of the default places below. |
| `ESKIU_CACHE_SIZE=MIB` | Size limit in MiB (default 1024); past it the least recently used objects are removed. |

Where the objects go, without `ESKIU_CACHE_DIR`:

- A program in a package (the nearest directory up from its root file with an `eskiu.toml`)
  keeps them in the package's `.eskiu-cache` directory. The directory holds a `.gitignore`
  that ignores it, so git never shows it, and removing the package removes its cache.
- Any other file keeps them in the user's cache directory: `~/Library/Caches/eskiu/obj` on
  macOS, `$XDG_CACHE_HOME/eskiu/obj` (else `~/.cache/eskiu/obj`) on Linux,
  `%LOCALAPPDATA%\eskiu\cache\obj` on Windows.

Each cache directory also has a `CACHEDIR.TAG`, which backup tools that honor it skip.
`--time` shows whether the compile was a `cache hit` or a `cache miss`. Deleting the
directory empties the cache. See [compile speed](../dev/compile-speed.md#the-object-cache)
for the key and measurements.

### Exit codes

Every command exits with:

| Code | Meaning |
|---|---|
| 0 | Success. |
| 1 | The work failed: a compile error, a failed link, a test that failed, a file that does not exist, `fmt --check` with files to format. |
| 2 | The command line is wrong: an unknown option, an option without its value or with a bad one (`-O9`, `--safe=nope`), no input file, a wrong argument to a command. Nothing was done. |

`eskiuc run` and `eskiuc run` inside a package exit with the program's own code instead
(128+N when signal N killed it). A usage error is one line on stderr:

```
error: unknown option '--frob' (see 'eskiuc --help')
error: fmt: unknown option '--frob' (see 'eskiuc fmt --help')
```

## Package Commands

`eskiuc` also manages packages: a directory with an `eskiu.toml` manifest, its
dependencies pinned in `eskiu.lock`. Fetching git dependencies needs the `git` command on
`PATH`. See "Packages" in the [getting started guide](getting-started.md#packages) for the
manifest format.

| Command | What it does |
|---|---|
| `eskiuc new NAME` | Create `NAME/eskiu.toml`, `NAME/src/main.esk` (hello world) and `NAME/.gitignore`. |
| `eskiuc add NAME --git URL [--rev REV]` | Add a git dependency to `eskiu.toml`, fetch it and update the lock. `REV` is a tag, branch or commit (default: the remote's default branch). |
| `eskiuc add NAME --path DIR` | Add a local dependency, used in place. |
| `eskiuc build [FILE.esk] [-o OUT] [FLAGS]` | Fetch what the lock names and is missing, then compile the package's main file to `build/NAME` (or `OUT`). With a `.esk` file named, compile that file instead (an example or a tool of the package, with its dependencies) to `build/FILE`. Other flags go to the compiler. |
| `eskiuc run [--dev] [--] [ARGS]` | Build, then run the program with `ARGS`. A leading `--dev` builds it with the development profile (`--safe` and `-Wall`). With a `.esk` file named, `eskiuc run file.esk` compiles that file as before. |
| `eskiuc test [FILTER...] [-j N] [--timeout S] [--json] [FLAGS]` | Compile and run the package's `tests/*.esk`, several at a time; a test passes when it exits with 0. The flags go to the compiler: `eskiuc test --dev` runs the tests with the `--safe` checks on. See [Running a package's tests](#running-a-packages-tests). |
| `eskiuc fetch` | Resolve every dependency again (fetching the git mirrors) and rewrite `eskiu.lock`. |

Git packages live in `~/.eskiu/pkgs/NAME/COMMIT`; set `ESKIU_HOME` to move that tree
(`$ESKIU_HOME/pkgs`). The lock is plain text, one tab-separated line per package, sorted
by name:

```
jsonx   git    https://github.com/x/jsonx   v1.2.0   <full commit hash>
myapp   root   .
util    path   /home/me/util
```

The `root` line is the package itself, so `import <myapp/helper>;` finds its own
`src/helper.esk`. Both compilers read this file to resolve package imports.

A package's prebuilt native libraries (`[native.NAME]`, see
[getting started](getting-started.md#native-libraries)) are downloaded with `curl` into
`~/.eskiu/native/SHA256` by the first build that links one (or the host ones by
`eskiuc fetch`) and checked against their SHA-256; a build in the package links them
with no `-L`, with either compiler (`eskiuc` or the self-hosted `eskiuc-esk`). The lock
does not list them: the manifest's SHA-256 names each one. `ESKIU_SYSTEM_LIBS=NAME,...`
links the system's library in place of the archive, and a build whose download fails
falls back to the system's library when there is one, with a warning (see
[getting started](getting-started.md#native-libraries)). A web build copies the files a
package lists in `[web] assets` next to its output ([web assets](getting-started.md#web-assets)).

### Running a package's tests

`eskiuc test` compiles each `tests/NAME.esk` to `build/tests/NAME` and runs it with no
arguments and stdin from the null device. Its compile and run output go to
`build/tests/NAME.log`; the end of the log is shown under a test that did not pass.

| Argument | Meaning |
|---|---|
| `FILTER` | Run only the tests whose name contains `FILTER`, matches it as a glob (`*`, `?`, `[...]`, quoted for the shell), or whose file it names (`tests/NAME.esk`). Several filters select the tests any one selects. |
| `-j N`, `--jobs N` | Run `N` tests at a time (default: half the logical cores, at least 1). Each job compiles its test, then runs it. |
| `--timeout SECONDS` | Kill a test that runs longer than this (default 60, `0` for no limit). The compile has no limit. |
| `--json` | Print one JSON object per test on stdout as it finishes, then a summary object, instead of the text lines. |
| other flags | Compiler flags (`-O2`, `--safe`, `-Wall`, ...), passed to every test's compile. An unknown one is a usage error. |

Each test ends with one status: `pass` (exit code 0), `fail` (another exit code),
`build-failed` (the compiler failed), `timeout` (killed after `--timeout`) or `crashed`
(killed by a signal: a segmentation fault, an abort). The exit code is 0 when every
selected test passed, 1 when one did not or when no test matched a filter, and 2 for a
wrong command line. The text output:

```
PASS inventory
FAIL cooldowns (exit 3)
    expected 4, got 3
FAIL bad_ptr (crashed: SIGSEGV)
FAIL loop (timeout after 60.0 s)
FAIL typo (build failed)
    error: tests/typo.esk:4:9: undefined variable 'x'
1 passed, 4 failed (1 build-failed, 1 timeout, 1 crashed) in 61.3 s, 5 jobs
failed: cooldowns bad_ptr loop typo
```

With `--json`, the lines come in the order the tests finish:

```
{"type":"test","name":"cooldowns","source":"/home/me/game/tests/cooldowns.esk","status":"fail","exit_code":3,"signal":null,"build_ms":1912.4,"run_ms":8.1,"log":"/home/me/game/build/tests/cooldowns.log"}
{"type":"summary","total":5,"passed":1,"failed":4,"build_failed":1,"timeout":1,"crashed":1,"duration_ms":61302.7,"jobs":5}
```

| Field | Value |
|---|---|
| `type` | `"test"`, or `"summary"` for the last line |
| `name` | the test's name, its file name without `.esk` |
| `source` | the path of its `.esk` file |
| `status` | `"pass"`, `"fail"`, `"build-failed"`, `"timeout"` or `"crashed"` |
| `exit_code` | the test's exit code (for `build-failed`, the compiler's); `null` for `timeout` and `crashed` |
| `signal` | the number of the signal that killed a `crashed` test, else `null` |
| `build_ms` | the compile's wall time in milliseconds |
| `run_ms` | the run's wall time in milliseconds; `null` when the build failed |
| `log` | the file holding the build and run output |
| `total`, `passed`, `failed` | (summary) tests run, passed, and not passed |
| `build_failed`, `timeout`, `crashed` | (summary) how many of the failed ended so |
| `duration_ms`, `jobs` | (summary) the whole run's wall time, and the tests run at a time |

### Publishing a package

A package is any git repository with an `eskiu.toml`; `eskiuc add NAME --git URL --rev TAG`
adds it from wherever it lives, so publishing one is tagging a release (`v0.1.0`). The
[package index](https://github.com/eskiu-lang/index) lists published packages so people can
find them: a pull request there adds `packages/NAME.toml` with the package's name (as in its
`eskiu.toml`), repository, a one-line description and its license, and a check reads the
repository before it is merged. The [package catalog](https://eskiu-lang.org/packages.html)
lists every package in the index. The packages the project maintains live in
[eskiu-packages](https://github.com/eskiu-packages) and are listed the same way.

## Running Tests

The automated suite is driven by `tests/run.sh`, which compiles, links, and runs every
`tests/*.esk` case (matching stdout against `*.expected`, smoke-running the rest, and
checking that `tests/errors/*.esk` are rejected and that `tests/warnings/*.esk` produce
their expected `-Wall` warnings). `eskiuc` links each test itself with
no `-l` flags, so the libraries programs imply (`#pragma link`, the C++ exception
runtime, pthread) are exercised too:

```bash
tests/run.sh                       # full suite against build/eskiuc
ESKIUC=/path/eskiuc tests/run.sh   # point at a specific compiler
```

CI runs the suite three ways: plain, then under UBSan and ASan (`SANITIZE=ubsan` /
`SANITIZE=asan`) as hardening gates. It also runs fuzzers from `tests/fuzz/` (see
`tests/README.md`): a generative fuzzer that mutates programs and fails on any compiler
crash, IR-verifier failure, or O0-vs-O2 runtime divergence, one that checks that invalid
programs are rejected with a located error, and one that runs the stdlib parsers under
ASan.

The four `--test-*` modes below expose individual compiler phases for manual inspection.

### `--test-lexer`

Tokenizes the input and prints every token with its type, value, and source location.

```bash
./build/eskiuc examples/hello.esk --test-lexer
```

Expected output (truncated):

```
Tokenizing: examples/hello.esk
========================================================
  Line   1, Col   1           EXTERN  'extern'
  Line   1, Col   8              INT  'int'
  Line   1, Col  12            IDENT  'printf'
  Line   1, Col  18           LPAREN  '('
  Line   1, Col  19           STRING  'string'
  ...
========================================================
Total tokens: 57
```

### `--test-parser`

Parses the token stream and prints the AST in indented form.

```bash
./build/eskiuc examples/hello.esk --test-parser
```

Expected output (truncated):

```
Parsing: examples/hello.esk
========================================================
Program
  ExternDecl: printf -> int
    Parameters:
      string fmt
      ... ...
  FunctionDecl: add -> int
    Parameters:
      int a
      int b
    Body:
      BlockStmt
        ReturnStmt
          BinaryExpr: +
            Left:
              IdentExpr: a
            Right:
              IdentExpr: b
  FunctionDecl: main -> int
    ...
```

### `--test-typechecker`

Runs semantic analysis. Prints `Type checking succeeded!` on success, or typed errors on failure.

```bash
./build/eskiuc examples/hello.esk --test-typechecker
```

Expected output on a valid file:

```
Type checking: examples/hello.esk
========================================================
========================================================
Type checking succeeded!
```

Example error output for `int f() { return 1.5; }`:

```
error: e.esk:1:11: return type mismatch: expected int, got double (cannot assign a floating-point value ('double') to integer type 'int' without an explicit cast (it drops the fraction))
========================================================
Type checking failed!
```

### `--test-codegen`

Runs the full pipeline through LLVM IR emission and prints the IR to stdout.

```bash
./build/eskiuc examples/hello.esk --test-codegen
```

A non-empty IR module with no `error:` lines on stderr indicates a passing build.

To batch-test all `.esk` files in the examples directory:

```bash
for f in examples/*.esk; do
  echo "=== $f ==="
  ./build/eskiuc "$f" --test-typechecker
done
```
