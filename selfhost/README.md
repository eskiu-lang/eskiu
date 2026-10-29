# The Eskiu compiler in Eskiu

This directory is a second implementation of the compiler, written in Eskiu. It covers the
whole pipeline (preprocessor, lexer, parser, type checker, async lowering, code generator)
and the full command line, and it behaves like the C++ `eskiuc` over the entire test
corpus. [`docs/dev/self-hosting.md`](../docs/dev/self-hosting.md) explains how that is
checked.

The C++ `eskiuc` is the released binary distribution (bundling LLVM and linking via the
system C compiler). This self-hosted compiler writes LLVM IR as text and explicitly relies
on `clang` (`$CLANG`, default `clang`) at runtime to assemble and link native binaries.

## Building

CMake builds it next to the C++ compiler when `clang` is available:

```bash
cmake -S . -B build && cmake --build build
./build/eskiuc-esk hello.esk -o hello
```

`eskiuc-esk` takes the same flags as `eskiuc` (`-o`, `-c`, `--target`, `-O0` to `-O3`,
`--safe`, the `--test-*` modes, `run`, `fmt`). It finds `stdlib/` through `$ESKIU_ROOT` or
the install layout, and uses `$CLANG` (default `clang`) to link.

## Files

| File | Role |
|---|---|
| `tokens.esk`, `lexer.esk` | Token kinds and the lexer |
| `preprocessor.esk` | `#define`, `#if`/`#ifdef`, `#pragma` and macro expansion, run before lexing |
| `ast.esk` | AST nodes and the `--test-parser` printer |
| `parser.esk` | Recursive-descent parser; follows `import` and merges the imported declarations |
| `sema.esk` | Type checker; imports its parts in order: `sema_base` (state, scopes, type helpers), `sema_assign`, `sema_nullable`, `sema_expr`, `sema_stmt` (and the entry point), `sema_flow` (definite return, `-Wall` flow checks), `sema_tooling` (editor modes) |
| `async_lower.esk` | Rewrites each `async` function into a frame struct and a state machine |
| `codegen.esk` | Writes LLVM IR as text; imports its parts in order: `codegen_base` (state, target, emission helpers), `codegen_types`, `codegen_ops`, `codegen_lval` (places, calls, builtins), `codegen_expr`, `codegen_stmt`, `codegen_decl` (functions, lambdas, type declarations), `codegen_cabi` (C ABI, program entry) |
| `eir.esk` | The EIR builder behind `--test-eir`, a port of `eir/eir_builder.cpp`; imports its parts in order: `eir_ty` (type spellings: the `ty::Type` grammar, `mangleTemplate`, `substType`), `eir_ir` (the EIR model and its text form), `eir_b_core` (state, scopes, types, folding, layout, bitfields, captures in generic bodies), `eir_b_conv` (conversions, interfaces, closures, `dbg`, generic instances), `eir_b_stmt`, `eir_b_expr`, `eir_b_build` |
| `fmt.esk` | The `fmt` subcommand |
| `esk_main.esk` | The compiler driver (`eskiuc-esk`) |
| `lex_main.esk`, `pp_main.esk`, `parse_main.esk`, `tc_main.esk`, `cg_main.esk` | Single-pass drivers used by the parity checks |
| `bigstack.esk` | Runs a driver on a large stack so deeply nested input does not overflow |

## Things to know when editing

- `fn`, `in` and `match` are keywords, so they cannot name a variable, parameter or field.
- `alloc<T>` returns zeroed memory, but initialize every `List` field of a struct with
  `List_init` before pushing to it.
- AST walks should only read the fields a node kind sets. A field a kind does not use is
  zero, which can look like a valid empty child.
- A change here usually needs the same change in the C++ compiler. Run the checks in
  `tests/selfhost/` before committing, including `cg_bootstrap.sh`, which rebuilds the
  compiler with itself.
