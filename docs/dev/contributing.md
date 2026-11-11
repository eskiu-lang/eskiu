# Contributing to Eskiu

Read [architecture.md](architecture.md) first. It walks through the pipeline (lexer,
parser, type checker, async lowering, a second type-checker pass, codegen), which every
change touches in some way.

## Build and check

```bash
git clone https://github.com/eskiu-lang/eskiu && cd eskiu
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(getconf _NPROCESSORS_ONLN)"
./build/eskiuc --version
tests/run.sh
```

On macOS, pass `-DLLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm` if CMake cannot find LLVM.
The build also produces `build/eskiuc-esk`, the compiler written in Eskiu, when `clang`
is available.

The `--test-lexer`, `--test-parser`, `--test-typechecker` and `--test-codegen` modes print
the output of each stage and are the quickest way to see what a change does.

Worth reading before a larger change: [architecture.md](architecture.md),
[design.md](design.md), `ast/ast.h`, `sema/type.{h,cpp}` (the `ty::Type` representation
shared by sema and codegen) and [self-hosting.md](self-hosting.md).

The site's docs pages (`site/docs/`) and package catalog are generated from the Markdown
in `docs/` by `site/build-docs.py`, which needs Python's `markdown` module. Install it in a
virtual environment and run the script with that environment's Python:

```bash
python3 -m venv ~/.venvs/eskiu-site
~/.venvs/eskiu-site/bin/pip install markdown
~/.venvs/eskiu-site/bin/python site/build-docs.py
```

Run it again after changing a page in `docs/`, and commit `site/` with the change.

## Two compilers

Eskiu has a C++ compiler (the released `eskiuc`) and a second one written in Eskiu under
`selfhost/`. A change to the language, its semantics or its diagnostics has to land in
both, in the same commit, and the parity checks in `tests/selfhost/` have to stay green.
Changes to the stdlib, the docs or the C++ driver alone usually do not touch `selfhost/`.

## Adding a language feature

1. Lexer and parser (`lexer/`, `parser/parse_{decl,stmt,expr}.cpp`). New type spellings go
   through `ty::Type::parse` in `sema/type.cpp`; do not add a second type-string parser.
2. A new AST node goes in `ast/ast.h` with a `visit` method on `ASTVisitor`; the build then
   fails until `ASTPrinter`, `TypeChecker` and `CodeGen` handle it.
3. Type checking in `sema/typecheck_*.cpp`, code generation in `codegen/codegen_*.cpp`.
4. The same steps in `selfhost/` (`parser.esk`, `sema.esk`, `codegen.esk`).
5. Tests in `tests/` (see below) and the docs that describe the behavior
   (`docs/lang/spec.md`, the book on the site, the VS Code grammar if the syntax changed).

Generic function bodies are not checked where they are declared. Each instance is checked
with its concrete type arguments after the main pass.

## Tests

`tests/run.sh` runs the whole regression suite. Each `.esk` file is classified
automatically: a file with a `NAME.expected` must print exactly that, one without it must
compile and exit 0, a file in `tests/errors/` must be rejected with the message on its
`EXPECT-ERROR:` line, and a file in `tests/warnings/` must produce the `-Wall` warnings
listed in it. Details are in [`tests/README.md`](../../tests/README.md).

Every fix comes with a regression test. CI also runs:

- the parity checks between the two compilers (`tests/selfhost/*.sh`) and the three-stage
  bootstrap;
- an `-O0` against `-O2` differential over the corpus (`tests/opt_differential.sh`);
- the fuzzers in `tests/fuzz/`: a generator whose programs are also emitted as C and
  compared with clang, a negative corpus with one injected error per program, and the
  stdlib parsers under AddressSanitizer;
- AddressSanitizer and UBSan builds, and a Windows job.

## Code style

- C++17, no dependencies beyond LLVM and the standard library.
- `camelCase` methods and members, `snake_case` locals.
- Comments only where the reason is not obvious from the code.
- AST nodes are `shared_ptr` (`ExprPtr`, `StmtPtr`, `DeclPtr`): struct-method bodies and template bodies
  are shared by several declarations, so do not convert them to `unique_ptr`.
- Commit subjects are short and imperative, with no trailing period.

## Proposals

Open an issue before a new language feature or a design change; scoped fixes can go
straight to a pull request. The roadmap is in [roadmap.md](roadmap.md).
