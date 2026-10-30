# Tests

Compiler regression tests. Run the whole suite with:

```bash
cmake --build build          # make sure the compiler is current
tests/run.sh
```

`run.sh` exits non-zero if anything fails, so it is safe to use as a CI gate.
Override the compiler or C linker with `ESKIUC=...` / `CC=...`.

## How a test is judged

The runner classifies every file automatically. There is no list to maintain.

| Kind  | Files | Pass condition |
|-------|-------|----------------|
| **run**   | `NAME.esk` **+** `NAME.expected` | compiles, links, runs, and stdout matches `NAME.expected` **exactly** |
| **smoke** | `NAME.esk` with no `.expected`   | compiles, links, and exits `0` (output not checked) |
| **error** | `errors/NAME.esk`                | `--test-typechecker` exits non-zero **and** the diagnostics contain the `EXPECT-ERROR:` substring from the file's first line |
| **lint**  | `warnings/NAME.esk`              | type-checks under `-Wall` and emits exactly the warnings named by its `// EXPECT-WARNING:` lines (none listed means none allowed) |

A `run` or `smoke` test may have a C companion `NAME.c` next to it. The runner compiles
it with `$CC` and links it into the test binary, which is how calls across the C ABI are
checked (`c_abi_struct.c`, `c_abi_try.c`, `c_abi_callback.c`, `c_abi_fnptr.c`,
`bitfield_c_layout.c`). Every test links through `eskiuc` with no `-l` flags, so the
libraries a program implies (`#pragma link`, the C++ runtime, pthread) are exercised too.

`run.sh` also runs the generated deep-input tests from `tests/deep/gen.sh`: a
100000-operand chain, 10000-deep nesting, programs past the 100000-level nesting
limit that must be rejected, and types just under and past the 1000-level type limit.

These are *honest* tests: a `run` test fails the moment the generated program
prints anything different, and an `error` test fails if the compiler ever starts
**accepting** code it should reject. (Verified by deliberately corrupting an
expected file and by adding a compilable file under `errors/`: both make
`run.sh` exit 1.)

## Coverage

Each test describes itself in the comment it starts with (an error test also by its
`EXPECT-ERROR:` text, a lint test by its `EXPECT-WARNING:` lines). `tests/index.sh`
prints the whole catalog from those comments as Markdown:

```bash
tests/index.sh > /tmp/catalog.md      # every run, smoke, error and lint test
tests/index.sh --check                # fails on a test that starts with no comment
```

`run.sh` runs the `--check`, so the catalog cannot fall behind the suite.

### Tests that need the system resolver

Every test runs on the machine alone (its servers listen on `127.0.0.1`) except
`dns_connect` and `fetch_dns`, which also need the system resolver to map `localhost` to
loopback and to fail for a `.invalid` name. A proxy that answers every name with one
address (fake-ip setups hand out `198.18.0.0/15`) breaks them; `ESKIU_RESOLVER_TESTS=0
tests/run.sh` skips them. CI keeps them on.

## Previously known issues (now fixed, and guarded by tests)

Two real compiler bugs were found while building this suite. Both are now fixed and
covered by exact-match `run` tests, so a regression would fail `run.sh`.

1. **`union` float member store**: *fixed.* `v.f = 3.14` stored the full 8-byte
   double into the union's offset-0 storage without truncating to `float`, so a
   later read got garbage (`126443839488.0`). The assignment path now coerces the
   RHS to the LHS member's declared scalar type. Guarded by `sizeof_union_ptr.esk`.

2. **No forward function resolution**: *fixed.* Codegen emitted functions in source
   order, so a call to a function defined later (or a `int b(int n);` forward
   declaration) failed with `Undefined variable or function`. Codegen now declares
   all prototypes in a pre-pass before emitting bodies, enabling call-before-define
   and mutual recursion. Guarded by `forward_decl.esk`.

## Other test scripts

Besides `run.sh`, these run in CI:

- `tests/opt_differential.sh`: every `tests/*.esk` must give the same exit code and
  stdout at `-O0` and `-O2`.
- `tests/safe_mode.sh`: `--safe` bounds checks trap on an out-of-range index and are off
  by default.
- `tests/nullable.sh`: the `?*T` checks and narrowing.
- `tests/type_zoo/snapshot.sh check`: the golden-IR oracle (see `docs/dev/debugging.md`).
- `tests/eir/check.sh [asm|c]`: every run test compiled by an EIR backend (ARM64
  assembly or C99, no LLVM code generator) and run as `run.sh` runs it; a test listed in
  `tests/eir/pass_<mode>.txt` that stops passing fails (`UPDATE=1` rewrites the list). CI
  runs both modes on macOS. See `docs/dev/eir-backends.md`.
- `tests/selfhost/*.sh`: parity between the self-hosted compiler and the C++ one
  (lexer, parser, preprocessor, type checker, codegen, C ABI, driver, EIR builder) and the
  bootstrap fixpoint; see `docs/dev/self-hosting.md`. `eir_parity.sh` compares
  `--test-eir` byte for byte over the run tests against the list in
  `eir_parity_pass.txt` (`UPDATE=1` rewrites it); given files, it shows each one's diff.

## Fuzzers

The fuzzers live in `tests/fuzz/`. Each run is deterministic for a given `--seed`, writes
what it finds to `tests/fuzz/findings/` (ignored by git), and exits non-zero when it finds
anything. Every compiler and program invocation runs under a timeout and a 4 GB resident
memory cap (`fuzz_util.py`), and a timeout or blowup counts as a finding. Parallel work is
limited to 4 jobs by default (`--jobs`).

**`eskiu_fuzz.py`** has two parts.

- The mutation and generation loop (`--iterations N`) feeds programs to `eskiuc` and
  reports crashes, IR verifier failures, hangs, and programs whose output differs between
  `-O0` and `-O2`.
- The C oracle (`--oracle N`, module `c_oracle.py`) generates programs inside a
  C-translatable subset of Eskiu: integers of every width and signedness, `bool` and
  `char`, casts, arithmetic, bitwise, shift and comparison operators, the ternary,
  `if`/`while`/`for`/`do`/`switch`/`break`/`continue`, nested blocks that shadow outer
  names, arrays, structs, pointers and pointer difference, pure and side-effecting
  functions, globals with constant initializers, `static` locals and `defer`. Each
  program is also emitted as C, built with `clang -O0 -fwrapv`, and the C++ `eskiuc` (at
  `-O0` and `-O2`) and the self-hosted `eskiuc-esk` must print exactly what it prints.
  This is the only check outside the Eskiu compilers, so it catches a bug both compilers
  share. Undefined behavior is avoided the same way on both sides (divisors and shift
  counts are guarded, indices masked, side effects kept where C fixes the order); `defer`
  has no C counterpart, so the C side runs the deferred statements at every exit.
- The second generator (`--oracle-ext N`, module `c_oracle_ext.py`) covers what the first
  does not: `float` and `double` arithmetic and conversions, structs passed and returned
  by value (small ones in registers, large ones through sret), arrays of structs, unions
  read through another member, and user variadic functions with mixed arguments. It is
  checked the same way (`c_oracle.check_program`).

```bash
python3 tests/fuzz/eskiu_fuzz.py --iterations 300 --seed 1           # mutation loop
python3 tests/fuzz/eskiu_fuzz.py --oracle-only --oracle 200 --oracle-ext 100 --seed 1   # the CI oracle gate
python3 tests/fuzz/eskiu_fuzz.py --oracle-only --oracle 20000 --seed 5   # a long run
python3 tests/fuzz/eskiu_fuzz.py --oracle-repro 5:1234    # print one program (Eskiu + C)
python3 tests/fuzz/eskiu_fuzz.py --oracle-reduce 5:1234   # shrink a finding
```

A finding is named `oracle_<build>-<kind>_<seed>_<index>` (for example
`oracle_self-diff_5_1234.esk` with its `.c` twin); `--oracle-reduce` deletes lines and
whole blocks while the finding still reproduces and writes `reduced_<seed>_<index>.*`.
Set `ESKIU_CLANG` to the clang to use, and `ESKIUC_ESK` if the self-hosted compiler is
not at `build/eskiuc-esk` (it is skipped when missing).

**`neg_fuzz.py`** is the negative corpus. It takes a program from the C-oracle generator
and injects exactly one error (about sixty kinds: undefined names, members of scalars,
bad calls and argument types, operators on structs or strings, bad casts, assigning to
an rvalue or a const, stray `break`/`continue`, duplicate locals and parameters,
out-of-range literals, division by a literal zero, unknown types, bad struct and array
literals, duplicate or non-constant `case` labels, non-constant globals and statics,
missing returns, unchecked `?*T` derefs, and more) at a random point of the program. Both
compilers must reject it without crashing, with an `error: file:line:col:` diagnostic on
the line of the injected error. Findings are `neg_<build>-<kind>_<seed>_<index>.esk`
(ACCEPT, CRASH, HANG, UNLOCATED, LOCATION).

```bash
python3 tests/fuzz/neg_fuzz.py --programs 300 --seed 1     # the CI gate
python3 tests/fuzz/neg_fuzz.py --programs 20000 --seed 7   # a long run
python3 tests/fuzz/neg_fuzz.py --repro 7:123               # print one program
```

**`stdlib_fuzz.py`** fuzzes the stdlib parsers under AddressSanitizer. Each target in
`tests/fuzz/stdlib/` (`json`, `base64`, `url`, `regex`, `hpack`, `http`, `fetch`, `multipart`,
`uuid_time`, `csv`, `dotenv`, `args`, `time_parse`) is an Eskiu program built with `eskiuc --asan`
that reads a batch of inputs and runs them through the parser, checking round trips where there
is one (base64 and url encode/decode and url_parse, hpack encode/decode, epoch/calendar
conversion, the fetch response parser fed whole and in small pieces, csv and dotenv write/parse,
and time_format/time_parse). The `args` target splits an input at NUL bytes into an argv. The driver mutates a
seed corpus (hand-written seeds plus string literals from the matching tests) and fails
on a crash, an ASan report, a broken invariant, a hang or a memory blowup, saving the
input to `stdlib_<target>_<seed>_<n>.bin`.

```bash
python3 tests/fuzz/stdlib_fuzz.py --inputs 1500 --seed 1   # the CI gate (a few seconds a target)
python3 tests/fuzz/stdlib_fuzz.py --seconds 300 --seed 7   # five minutes per target
python3 tests/fuzz/stdlib_fuzz.py --targets hpack --replay tests/fuzz/findings/x.bin
```

## Linux smoke from a Mac (pre-release)

`tests/linux_docker.sh` checks a release on Linux without a Linux build of `eskiuc`. It is
a manual pre-release check, not a CI gate (it needs Docker). The compiler cross-compiles
every run and smoke test and the self-hosted drivers for `$TRIPLE` (default
`aarch64-unknown-linux-gnu`), and a container (`ubuntu:24.04` plus gcc, built once and
cached) links and runs them:

- each run test must print exactly its `.expected`, each smoke test must exit 0;
- `esk_main --test-typechecker` (over `tests/*.esk` and `tests/errors/*.esk`) and
  `--test-codegen` (over `tests/*.esk`) must not crash or time out;
- `cg_main` compiles `cg_main.esk` on Linux, the host clang assembles that IR, and the
  result must emit the same IR again (the bootstrap fixpoint, on Linux).

A test that is a known failure on the target (listed in the script, with its CHANGELOG
entry) is reported as XFAIL and does not fail the run.

```bash
CLANG=$(brew --prefix llvm@22)/bin/clang ESKIUC=build/eskiuc tests/linux_docker.sh
TRIPLE=x86_64-unknown-linux-gnu tests/linux_docker.sh    # x86-64, emulated on Apple silicon
TESTS="regex http2_server" tests/linux_docker.sh         # only these tests
```

## Adding a test

Start every test with a `//` comment that says what it checks: `run.sh` fails without it
(`tests/index.sh --check`).

- **Positive, deterministic output:** add `NAME.esk` and `NAME.expected` (the exact
  stdout). Generate the expected file from a *known-correct* run, then read it to
  confirm it is right before committing.
- **Positive, non-deterministic / smoke only:** add `NAME.esk` with no `.expected`.
- **Negative:** add `errors/NAME.esk` whose first line is
  `// EXPECT-ERROR: <substring of the diagnostic>`.
- **Lint:** add `warnings/NAME.esk` with one `// EXPECT-WARNING: <substring>` line per
  expected `-Wall` warning (no such line means the file must produce no warning).
