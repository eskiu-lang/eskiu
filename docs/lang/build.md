# Building Eskiu from Source

## Requirements

| Dependency   | Minimum                                | Tested      |
| ------------ | -------------------------------------- | ----------- |
| LLVM         | 21                                     | 22.x        |
| CMake        | 3.20                                   | 3.x         |
| C++ compiler | C++17 (clang++ recommended, g++ works) | clang++ 17+ |
| git          | any                                    | n/a         |

LLVM is the only non-trivial dependency. The compiler links the `support`, `core`, `irreader` and native code generation components, plus the AArch64, X86, ARM and WebAssembly backends used for cross-compilation.

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

### Incremental builds

After changing source files, just re-run the build step; CMake tracks dependencies:

```bash
cmake --build build -- -j$(nproc)
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
Eskiu 0.12.0 (LLVM 22.x.x)
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

---

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
| `eskiuc build [-o OUT] [FLAGS]` | Fetch what the lock names and is missing, then compile the package's main file to `build/NAME` (or `OUT`). Other flags go to the compiler. |
| `eskiuc run [ARGS]` | Build, then run the program with `ARGS`. With a `.esk` file named, `eskiuc run file.esk` compiles that file as before. |
| `eskiuc test` | Compile and run each `tests/*.esk` of the package; a test passes when it exits with 0. |
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
`~/.eskiu/native/SHA256` by the first build that links one (or all of them by
`eskiuc fetch`) and checked against their SHA-256; a host build in the package links them
with no `-L`. The lock does not list them: the manifest's SHA-256 names each one.

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
