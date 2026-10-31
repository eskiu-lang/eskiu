# Self-Hosting

Eskiu has two implementations of its compiler. The released `eskiuc` is written in C++17
and links LLVM. The second, under `selfhost/`, is written in Eskiu: it covers the same
pipeline and command line, writes LLVM IR as text, and calls `clang` to assemble and link
it. CMake builds it as `eskiuc-esk`. See [`selfhost/README.md`](../../selfhost/README.md)
for its layout.

```
preprocessor → lexer → parser → sema → async lowering → codegen (LLVM IR text) → clang
```

The C++ compiler is the reference. Every change to the language lands in both compilers
in the same commit, and CI checks that they agree.

## How agreement is checked

The ground truth differs by pass, so the check does too:

| Pass | Check |
|---|---|
| Preprocessor, lexer, parser | Byte-exact comparison with `eskiuc --test-lexer` and `--test-parser` |
| Type checker | Same accept or reject verdict as `--test-typechecker`, with the same message and `line:col` |
| Code generator | Behavioral: build the program with each compiler, run both, compare exit code and output. LLVM renumbers values and folds constants, so the IR itself is not compared |
| C ABI | The C signatures each compiler lowers for `extern` structs and callbacks, per target |
| EIR builder | Byte-exact comparison with `--test-eir` over the run tests (`eir_parity.sh`; the tests that match are listed in `eir_parity_pass.txt`, and one that stops matching fails the check) |

The scripts are in `tests/selfhost/` and all run in CI. The code-generator check covers
every runnable program in `tests/`. The fuzzers in `tests/fuzz/` also run generated
programs through both compilers.

## The bootstrap fixpoint

The self-hosted compiler is also checked against itself. `cg_bootstrap.sh` builds it in
three stages: the C++ `eskiuc` builds `cc0`, `cc0` builds `cc1`, and `cc1` builds `cc2`.
The check passes when `cc1` and `cc2` produce identical IR for the compiler's own source.
The binaries themselves are not compared, since a Mach-O file embeds a UUID and signature
that change between identical builds.

A fixpoint only exercises the parts of the language the compiler's own source uses. It
does not show that the rest works, which is why the behavioral check runs over the whole
test corpus as well.

## Running the checks

```bash
tests/selfhost/lex_parity.sh   --full   # lexer
tests/selfhost/pp_parity.sh    --full   # preprocessor
tests/selfhost/parse_parity.sh --full   # parser
tests/selfhost/tc_parity.sh             # type checker, valid and invalid programs
tests/selfhost/cg_parity.sh             # code generator
tests/selfhost/corpus_parity.sh         # every runnable test through eskiuc-esk
tests/selfhost/driver_parity.sh         # building with -o
tests/selfhost/run_parity.sh            # eskiuc-esk run
tests/selfhost/fmt_parity.sh            # eskiuc-esk fmt
tests/selfhost/cabi_parity.sh           # C ABI lowering per target
tests/selfhost/cg_bootstrap.sh          # three-stage fixpoint
```

They take the C++ compiler from `ESKIUC` (default `build/eskiuc`) and clang from `CLANG`.

## Execution backend and distribution model (Roadmap to 1.0)

`eskiuc-esk` serves as a full self-hosting validation engine and bootstrap compiler:

- **Backend mechanism:** It emits structured textual LLVM IR (`.ll`) directly from Eskiu and invokes an external C toolchain (`clang` via `$CLANG`) to assemble machine code and link system libraries. With `--backend=eir` it builds through EIR instead: its C99 emitter (`selfhost/eir_c.esk`, a port of `eir/eir_c.cpp` whose `--emit-c` output is byte-identical over every run test, `tests/selfhost/eir_c_parity.sh`) writes C that any C compiler builds (`$CC`, then `$CLANG`, then `cc`), on every target; `tests/eir/check.sh driver` runs the run tests that way.
- **Runtime prerequisites:** `clang` is required on `$PATH` to produce executable binaries through LLVM IR; `--backend=eir` needs only a C99 compiler. CMake automatically detects `clang` and gracefully skips the self-hosted build target when clang is unavailable (such as under MSVC).
- **Distribution strategy for 1.0:** The primary shipped binary release remains the C++ `eskiuc` (which bundles LLVM and links via system `cc`/`clang`/`gcc`). For 1.0, `eskiuc-esk` formally declares `clang` as an explicit runtime requirement for self-hosted execution, keeping the self-hosted compiler lightweight, transparent, and free of heavyweight C++ LLVM library linkage.
