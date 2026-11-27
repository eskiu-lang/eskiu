# Compile speed

Where the C++ `eskiuc` spends its time on a real program, how to measure it again, and what
has been done about it. The reference program is `examples/kit-gallery` of the `ui`
package (eskiu-packages/ui): 153 lines of its own, about 47,000 lines of LLVM IR once the `ui`
library and the standard library it imports are in, so it stands for an app whose compile
time is the library's. The measured command is the `ui serve` rebuild, an `-O0` object:

```bash
cd eskiu-ui/examples/kit-gallery
eskiuc src/main.esk -c -o /tmp/kg.o
```

## Measuring

`eskiuc --time` (or `ESKIU_TIME_PHASES=1` in the environment) makes the compile pipeline
print the wall time of each phase on stderr, then the link and the total (`phase_timer.h`;
`phaseMark` calls in `main_compile.h` and `CodeGen::generateCode`):

```
phase start                 0.0 ms  (total      0.0 ms)
phase load+parse           23.0 ms  (total     23.0 ms)
phase typecheck            27.6 ms  (total     50.6 ms)
phase irgen                29.8 ms  (total     80.4 ms)
phase ir-postpasses         0.7 ms  (total     81.1 ms)
phase verify                4.1 ms  (total     85.2 ms)
phase optimize              0.0 ms  (total     85.2 ms)
phase emit-object         193.0 ms  (total    278.2 ms)
```

A build that links adds `phase link` (the C driver and the linker) and every compile ends
with `phase total`; `eskiuc run` prints the total before the program starts, so it does not
count the program's own run time.

`load+parse` is preprocessing, lexing and parsing the program and every file it imports;
`async-lower` and `re-check` appear only for a program with an `async` function (below);
`irgen` is the AST to LLVM IR walk, `ir-postpasses` the module passes after it (packed
alignment, TBAA, the wasm aggregate rewrite), `emit-object` LLVM's back end and the object
writer.

For a profile, build with line tables and take a dSYM, then record one compile with
Instruments' Time Profiler and summarize it with `tools/xctrace_top.py` (several traces can
be summed: the sample interval is 1 ms, so a 0.3 s compile gives about 300 samples):

```bash
cmake -S . -B build-prof -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-gline-tables-only
cmake --build build-prof -j4 --target eskiuc
dsymutil build-prof/eskiuc -o build-prof/eskiuc.dSYM
xcrun xctrace record --template 'Time Profiler' --output t.trace --launch -- \
    build-prof/eskiuc src/main.esk -c -o /tmp/kg.o
xcrun xctrace symbolicate --input t.trace --dsym build-prof/eskiuc.dSYM
xcrun xctrace export --input t.trace \
    --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]' > tp.xml
tools/xctrace_top.py tp.xml 40                          # whole run
tools/xctrace_top.py tp.xml 40 --under 'Parser::fail'   # only stacks through a frame
```

`llc -O0 -filetype=obj -mcpu=apple-m1 kg.ll -time-passes` (on the IR from `--test-codegen`)
shows how the back end's time splits between its passes.

## The breakdown, before and after

Kit gallery, `-c` at `-O0`, M-series Mac, wall time, median of 5 (2026-11-09). "No build
type" is what `cmake -S . -B build` produced before this work: CMake then compiles with no
optimization at all.

| Phase | Before, no build type | Before, Release | After, Release |
|---|---|---|---|
| load + parse | 261 ms | 140 ms | 23 ms |
| type check | 304 ms | 45 ms | 28 ms |
| type check again (after the async lowering) | 299 ms | 43 ms | not run |
| IR generation | 230 ms | 38 ms | 29 ms |
| IR passes + verifier | 21 ms | 5 ms | 5 ms |
| LLVM back end + object | 218 ms | 193 ms | 191 ms |
| **total (`time`)** | **1.36 s** | **0.47 s** | **0.29 s** |
| `--test-typechecker` | 0.60 s | 0.19 s | 0.06 s |
| `--symbols` (parse with imports) | 0.29 s | 0.15 s | 0.03 s |

The compiler's own source (`selfhost/esk_main.esk`, 69,000 lines with its imports) went from
10.9 s (no build type) to 1.6 s, of which 0.95 s is the back end.

## What was changed, and what each gave

Each line is measured on the kit gallery in a Release build, the object byte-identical
before and after.

1. **Release by default** (`CMakeLists.txt`). Without `-DCMAKE_BUILD_TYPE` CMake passes no
   `-O` flag, and that compiler is about three times slower (1.36 s against 0.47 s). The
   documented `cmake -S . -B build` now builds Release; `-DCMAKE_BUILD_TYPE=Debug` still
   gives a debug build.
2. **No thrown exception per statement in the parser** (`Parser::declarationRuledOut`,
   `Parser::templateCallRuledOut`, `Parser::scanTypeArgs`): parse 140 ms to 25 ms. The
   parser tries a block item as a declaration first and, for `f(x);`, `a.b = c;`,
   `List_push<T>(&l, v);`, gives up with a thrown `ParseError`; likewise every `i < n` was
   first tried as an explicit generic call `i<n>(...)`. A throw costs about 40 us on macOS
   (the unwinder looks up each frame's unwind section through dyld): the compiler's own
   source threw about 4,000 times after the first predicate, and parsed 150 ms faster
   once the second one removed them. The two predicates read the tokens ahead and skip the
   attempt when it cannot succeed (a declaration needs a type then a name or `operator`;
   a generic call needs `<` types `>` then `(` or `{`), answering "try it" whenever the
   tokens leave a doubt, so the AST is the same (`--test-parser` is byte-identical over
   every test, example, stdlib, self-host and `ui` file).
3. **One type-checker run for a program without `async`** (`programHasAsync` in
   `main_modes.h`): 43 ms. The async lowering rewrites each `async` function into a frame
   struct, a resume function and a constructor, which must be checked before codegen reads
   their types, so the checker ran a second time over the whole program. A program with no
   `async` function is left as it was, and the first run's results are what codegen needs.
   The one difference the corpus showed: `sizeof(expr)` inside a lambda captured the
   variables of `expr`, since the checker rewrites the node to `sizeof(T)` only after
   visiting the operand; the second run saw the rewritten node and captured nothing. The
   checker now drops those captures when it rewrites (`visit(SizeofExpr)`). With that,
   `--test-codegen` prints the same IR as before over all of `tests/` and `examples/`. The
   EIR back end keeps the second run (its builder needs the per-instance records only a
   checker with `recordInstanceTypes` keeps).
4. **`nodeCast<T>` for `dynamic_cast<T*>` on AST nodes** (`ast/ast.h`): type check 45 ms to
   28 ms, IR generation 38 ms to 28 ms. Every pass tests node kinds with chains of
   `dynamic_cast`, and libc++abi's `dynamic_cast` walks the RTTI (a failing cast costs tens
   of nanoseconds), which made it the type checker's largest cost. Each node class is a
   leaf, so `nodeCast` compares the node's kind (one virtual call, `nodeKind()`, declared by
   `ESKIU_NODE_KIND` in each class) and `static_cast`s. Marking the classes `final` did not
   let clang do the same.
5. **`Parser::check` / `match` read the token in place** instead of copying it through
   `peek()`, and `match` takes an `initializer_list` instead of building a vector per call.
   A few milliseconds.

All five are in the C++ compiler only: the self-hosted parser does not use exceptions to
backtrack, and none of them changes what a program means.

## The object cache

A program's tests are often built one file at a time (`make check`, `eskiuc test`), each
from the same libraries and mostly the same sources, and at `-O2` the LLVM back end is
nearly all of each compile. With `--cache` (or `ESKIU_CACHE=1`), a compile that writes an
executable or an object file keeps the object it emitted, and the next compile with the
same inputs copies it instead of type checking, generating, optimizing and emitting. The
link still runs. `build_cache.{h,cpp}` is the cache, `objectCacheKey` in
`main_build_options.h` the key, `compileProgram` and `buildFromCache` in `main_compile.h`
its use. The self-hosted compiler has none.

**Key.** After loading and parsing (imports resolved, so the key sees which files the
program is made of), a BLAKE3 hash over:

- the compiler: its version, LLVM's version, and a hash of its own executable's contents
  (computed once per executable path, size and modification time, and kept in
  `compiler-ids/` in the cache directory), so a rebuilt compiler misses.
  `ESKIU_CACHE_COMPILER_ID` replaces it (the tests use it);
- the target: `--target`, the host's default triple and its OS version (macOS objects
  carry it);
- the command line as given, without `-o FILE`, `--time`, `--cache` and `--no-cache`: every
  other option that can change the object (`-O`, `--safe`, `-W` flags, `--target`,
  `--mcpu`, ...) is in it, spelled as given, so a reordering misses but never hits
  wrongly. Whether the output is a shared library (decided by the `-o` name) and whether
  this is `eskiuc run` are added;
- the working directory and the root of the files that get warnings (the package root);
- every source file the loader read, in the order it read them: the path as the compiler
  spelled it (which `__FILE__` and diagnostics show) and a hash of the contents. That is
  the inputs, their imports, the standard library modules and package files. An
  `eskiu.lock` or `ESKIU_ROOT` that resolves an import to another file changes the paths.

Nothing else is read while compiling: the preprocessor has no `#include` of other files,
no `__DATE__`/`__TIME__` and no environment access, and `#pragma link` libraries come from
the parse, which runs on a hit too.

**Entries and writes.** `KEY.o` and `KEY.meta` (what the link needs: whether the program
uses the C++ exception runtime or threads, the macOS version of the object, and the
warnings) in `$ESKIU_CACHE_DIR`, else the package's `.eskiu-cache` (the nearest directory up
from the root file with an `eskiu.toml`), else the user's cache directory
(`objcache::userDirectory()`: `~/Library/Caches/eskiu/obj`, `~/.cache/eskiu/obj`,
`%LOCALAPPDATA%/eskiu/cache/obj`). A package's objects serve only it (the key covers every
source), so they live and go with it.
Each file is written to a unique temporary file and renamed into place, the object first,
so concurrent builds (`make -j`, `eskiuc test -j`) never see half an entry; two builds of
the same key write the same bytes. A reader that cannot copy the object (evicted meanwhile)
compiles as on a miss. Any failure to read or write the cache only makes the compile not
use it.

**Eviction.** After each store, when the directory holds more than `ESKIU_CACHE_SIZE` MiB
(default 1024), the entries least recently used (a hit touches `KEY.meta`) are removed
until it holds 90% of that. Temporary files older than an hour (from a killed build) go too.

**Diagnostics.** The compile's standard error from type checking to the object (its
warnings; a compile with errors is not stored) is kept in `KEY.meta` and printed again on a
hit, so `-Wall` output does not depend on the cache. `--diagnostics=json` passes the text
through its converter on both paths, so the JSON lines are the same.

**On by default.** The key covers everything a compile reads (above), so a hit is the
object the compile would make. `--no-cache`, `ESKIU_NO_CACHE=1` or `ESKIU_CACHE=0` turn it
off (a Makefile's compiles inherit the variable).
`--time` shows `phase cache-key` (the key's time), `phase cache   hit KEY` or `miss KEY`,
then `cache-copy` (hit) or `cache-store` (miss).

### Measured on a game's tests

The 45 compiles of a game's `make check` (about 20,000 lines of its own plus the `ui`,
`scene` and `gpu` packages; 44 tests at `-O2`, one at `-O0`), run 4 at a time with
`xargs -P 4` from a copy of the project, outputs and cache in a scratch directory. Apple
M4 Pro (14 cores, 24 GB), macOS, an `eskiuc` Release build against Homebrew LLVM 23.1,
2026-11-25; only the compiles are timed, not the test runs.

| Run | Wall time |
|---|---|
| no cache | 520.6 s |
| `--cache`, empty cache (every compile a miss and a store) | 758.8 s |
| `--cache`, nothing changed (45 hits) | 1.2 s |
| `--cache`, one test file edited (1 miss, 44 hits) | 6.7 s |

The empty-cache run is slower than the no-cache run because the machine was busier then
(other builds ran alongside): on one compile the cache adds the key (0.3 ms; 90 ms on
the first compile after the compiler is rebuilt, which hashes its executable) and the
store (15 ms). A hit is load and parse (40 ms), the key, copying the object (6 ms) and the
link (50 ms): about 0.1 s against 4 to 95 s for the compile it replaces. The 45 objects
take 25 MB. An edit to a file every test imports (`src/match.esk`, 24 of them) misses
for each of them.

These numbers were taken before the copies of large records moved to memory (0.17.0): then
the back end dominated, `tests/battle_integration.esk` spending 91 s in `emit-object`
(56 s with the released 0.16.3 on LLVM 22). With that change the same file compiles in
3.8 s at `-O2` (2.0 s optimizing, 1.6 s emitting), and the game's whole `make check` went
from 1290 s to 168 s with no cache at all.

To measure again: `--time` on one compile shows `cache-key`, `cache hit`/`miss`,
`cache-copy` or `cache-store`; for a suite, time the commands with an empty
`ESKIU_CACHE_DIR`, then again, then after touching one file.

## What is left

- **The back end is two thirds of the time.** GlobalISel at `-O0` on AArch64 takes 190 ms
  for the gallery's 47,000 lines of IR, about what `llc -O0` takes (FastISel is slower here:
  it falls back to SelectionDAG). It scales with the IR, so the lever is emitting less IR.
  Dropping functions nothing reaches is not safe in general: a host calls the app's
  functions by name (`ui_start`, `ui_dispatch`, ... through `dlsym` in a `ui serve` build),
  and C companions call any Eskiu function, so every function with external linkage is a
  root. Splitting the module and running the back end on several threads (as LLVM's LTO
  does) would cut the wall time, at the cost of merging the objects.
- **Re-parsing and re-checking the libraries on each build.** The gallery's own code is 153
  lines, so nearly all of its 80 ms of parsing, checking and IR generation is the `ui`
  library and the stdlib, the same on every rebuild. A compiler that stays up between
  rebuilds (`ui serve`) or a cache of the checked library would remove most of it.
- **A program with `async` still checks twice.** Checking only the declarations the
  lowering produced needs the checker to take the first run's state for the rest.
- **String-typed types.** In the type checker the next costs are `ty::Type::parse` and
  `normalizeType` over type spellings (about a quarter of its time), with no single hot spot.
