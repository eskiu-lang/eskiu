# Debugging the Eskiu Compiler

How to diagnose issues in the lexer, parser, type checker, and code generator.

## Test Modes Reference

| Flag | What it does | What it outputs | When to use |
|---|---|---|---|
| `--test-lexer` | Tokenizes source, stops after lexing | One token per line with type, lexeme, line, and column | Diagnosing unrecognized tokens, bad keyword handling, or unexpected token splits |
| `--test-parser` | Tokenizes + parses, stops before type checking | Indented AST via the ASTVisitor/ASTPrinter pass | Diagnosing missing nodes, wrong nesting, or silently skipped declarations |
| `--test-typechecker` | Tokenizes + parses + type checks | Errors as `error: file:line:col: message`, or "Type checking succeeded!" | Diagnosing undefined variables, type mismatches, struct field errors |
| `--test-codegen` | Full pipeline except linking | LLVM IR text via `module->print()` | Diagnosing wrong IR types, missing terminators, LLVM verification failures |

Each mode exits 0 on success; diagnostics go to stderr as `error: file.esk:line:col: message`. Pick the earliest mode whose output is already wrong: a defect at the lexer level is much easier to read in `--test-lexer` than in the final IR.

## Golden-IR Snapshot Oracle

`tests/type_zoo/snapshot.sh` is the codegen-regression guard. The type zoo is a corpus of programs that exercise the breadth of the type grammar; the script emits each program's LLVM IR and compares it against the checked-in baseline under `tests/type_zoo/golden/`.

- **Check** (`tests/type_zoo/snapshot.sh check`, the CI mode; rebuild `eskiuc` first): regenerate the IR for every zoo program and diff it against `tests/type_zoo/golden/`. Any difference fails the run. That diff *is* the diagnosis: it shows exactly which construct now lowers differently.
- **Capture** (`tests/type_zoo/snapshot.sh capture`, after an intentional codegen change): regenerate and overwrite the golden files, then review the diff before committing so an unintended change can't slip in disguised as an intended one.

Use this whenever you touch codegen, the `ty::Type` IR, or anything in `sema/` that feeds the resolved type table: a behavior-preserving refactor must produce byte-identical golden IR.

## Fuzzer and the O0-vs-O2 Differential

`tests/fuzz/eskiu_fuzz.py` is a generative + mutation fuzzer. It synthesizes (and mutates) `.esk` programs and runs them through a **differential oracle**: each program is compiled at `-O0` and at `-O2` and executed, and any divergence in output (or a crash at one level but not the other) is reported as a miscompile.

Why this catches real bugs: a correct program must produce the same observable result regardless of optimization level. When the two disagree, the front end emitted IR that was *accidentally* correct at `-O0` but wrong once LLVM's optimizer was allowed to exploit it (e.g. an undef value, a wrong integer width, or a missing extension that `-O0` happened to mask). This is precisely the class of latent miscompile the v0.2.4 single-resolver work fixed. When the fuzzer flags a case, minimize the generated program, then bisect with `--test-codegen` to find the construct whose IR is wrong.

Some generators carry their own **expected stdout** instead of relying on the O0/O2 differential, notably the backslash-newline-in-comments/strings generators, which catch *uniformly* mis-lexed programs (where O0 and O2 agree but are both wrong, e.g. a `//` comment ending in `\` swallowing the next line). A mismatch against the known output (or a build failure) is the finding.

The O0/O2 differential cannot see a bug that both levels share, or one that the C++ and self-hosted compilers share. The **C oracle** (`--oracle N`, `tests/fuzz/c_oracle.py`) covers that case: it generates programs in a C-translatable subset, emits each one as C as well, and compares the output of both Eskiu compilers with what `clang -O0 -fwrapv` prints. `--oracle-repro SEED:INDEX` prints one program in both languages and `--oracle-reduce SEED:INDEX` shrinks a finding. For errors rather than miscompiles, `tests/fuzz/neg_fuzz.py` injects one error into a generated program and flags a compiler that accepts it, crashes, or reports it on the wrong line. `tests/README.md` has the command lines.

The corpus-wide counterpart is `tests/opt_differential.sh`: it compiles every `tests/*.esk` with `eskiuc -O0` and with `eskiuc -O2` (the `-O0`/`-O1`/`-O2`/`-O3` flag runs the LLVM middle-end before code generation) and fails on any exit/stdout divergence. Reach for it when diagnosing a bug that only appears under optimization: run it, then re-run the one failing program at each level and diff `--test-codegen` output. It caught a float-closure return-type miscompile in 0.3.1.

## Resolver Consistency: `ESKIU_RESOLVER_DEBUG`

The single resolver makes codegen consume the type checker's resolved-type table instead of re-deriving types. The table doesn't annotate *every* expression, so codegen keeps a structural fallback (`deriveExprEskiuType`). To guard against the two ever disagreeing (the latent-miscompile class), set `ESKIU_RESOLVER_DEBUG=1`: whenever the table *has* an entry, codegen also runs the derivation and prints `[resolver-disagree] table=… derive=… <ExprKind>` on any semantic difference (benign representational noise, the `struct:`/`interface:` tag and alias spelling, is normalized away first).

```
for f in tests/*.esk; do ESKIU_RESOLVER_DEBUG=1 build/eskiuc --test-codegen "$f" 2>&1 >/dev/null; done | grep resolver-disagree
```

Across the corpus this is silent except for one benign enum-as-underlying-int case (`Color` vs `int`), i.e. the two evaluators agree on every behavior-affecting type. Run it after any change to `sema/` type resolution or codegen's type derivation; a new disagreement is the diagnosis.

## Runtime Memory / UB: `--asan` and `--ubsan`

For defects that only show up at run time:

- `--asan` instruments the build with AddressSanitizer (via the LLVM pass manager) and links the matching compiler-rt runtime. It catches use-after-free, out-of-bounds, and leaks at the point of the bad access.
- `--ubsan` inserts trapping checks for undefined behavior (e.g. out-of-bounds indexing) with no runtime dependency.

Both compose with `eskiuc run` (flags go before the script: `eskiuc run --asan f.esk`). Reach for these when a program type-checks, compiles, and produces wrong or nondeterministic results at run time rather than a compile-time diagnostic. They are also run as a CI gate over the test corpus.

## Editor Tooling: `--hover-at` / `--definition-at`

The VS Code extension drives two CLI flags that are also useful for manual diagnosis:

- `--hover-at LINE:COL` runs the pipeline through the type checker and prints the inferred Eskiu type of the innermost AST node containing that position (read straight from the resolved expression-type table). Use it to confirm what type the resolver actually assigned an expression.
- `--definition-at LINE:COL` finds the symbol at that position and prints the `file:line:col` where it was declared.

A type name works in all three queries (`--hover-at`, `--definition-at`, `--references-at`)
wherever a type is written: parameter, return, local, global and field types, the pointer,
nullable, array and slice forms, generic arguments, casts, `sizeof(T)`, struct literals,
constraints and alias targets. The parser records each named type token of the primary input
(`Program::typeRefs`, built by `Parser::noteTypeRef`; a backtrack in `rewindTo` drops the ones
read past it), and `TypeChecker::typeDefAt` resolves it through `typeDefs`, the declared
structs, unions, enums, interfaces and aliases. Hover prints the declaration text
(`struct Point`, `type A = Point`). The self-host mirrors it: `p_note_type` / `p_rewind` in
the parser fill `Program.tr_*`, and `sema_tool_type_at` (`selfhost/sema_tooling_types.esk`)
answers in `run_tool`.

Because `--hover-at` reports the resolver's own answer, it is the quickest way to see whether a bug is "the type checker resolved the wrong type" versus "codegen lowered a correctly-resolved type wrongly."

## Note: the type checker runs twice (single resolver)

The pipeline runs the type checker, then the async transform rewrites the AST, then the type checker is **re-run on the transformed AST** to produce the per-expression `ty::Type` table that codegen consumes. Codegen does not re-derive expression types; it reads that table, and `getTypeFromString` interprets type spellings through the single `ty::Type::parse` grammar.

This gives a defined diagnosis path for codegen bugs:

- If `--hover-at` (the resolver's table) shows the right type but the IR is wrong, the defect is in codegen's lowering of a correctly-resolved type.
- If the table itself shows the wrong type, the defect is in the type checker / `ty::Type` resolution.
- An **O0-vs-O2** divergence (fuzzer) or a **table-vs-codegen** mismatch (hover type correct, IR wrong) both point at codegen consuming the table incorrectly rather than at the resolver. Because there is a single resolver, "the two evaluators disagreed" is ruled out wherever the table has an entry (and `ESKIU_RESOLVER_DEBUG` checks the fallback), narrowing the search.
