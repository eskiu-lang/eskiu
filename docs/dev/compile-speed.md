# Compile speed

Where the C++ `eskiuc` spends its time on a real program, how to measure it again, and what
has been done about it. The reference program is `examples/kit-gallery` of the `ui`
package (eskiu-lang/ui): 153 lines of its own, about 47,000 lines of LLVM IR once the `ui`
library and the standard library it imports are in, so it stands for an app whose compile
time is the library's. The measured command is the `ui serve` rebuild, an `-O0` object:

```bash
cd eskiu-ui/examples/kit-gallery
eskiuc src/main.esk -c -o /tmp/kg.o
```

## Measuring

`ESKIU_TIME_PHASES=1` makes the compile pipeline print the wall time of each phase on
stderr (`phase_timer.h`; `phaseMark` calls in `main.cpp` and `CodeGen::generateCode`):

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
   `main.cpp`): 43 ms. The async lowering rewrites each `async` function into a frame
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
