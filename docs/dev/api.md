# Eskiu Compiler C++ API Reference

This document describes the public C++ API of the Eskiu compiler components. It
is intended for developers embedding the compiler as a library or building
additional analysis and transformation passes on the existing infrastructure.

The compiler is built with C++17 and links against LLVM. All public headers live
under the project root; include paths are relative to it. The pipeline is a chain
of four passes (lex, parse, type-check, code-generate) with an async-lowering
pass between type checking and code generation. Each pass is an independent class
that can be driven on its own.

---

## Lexer

Header: `lexer/lexer.h`

```cpp
class Lexer {
public:
    explicit Lexer(const std::string& source,
                   std::map<std::string, Macro>* macros = nullptr,
                   const std::string& filename = "",
                   const PPImportHook* importHook = nullptr);

    Token next_token();          // produce the next token; returns EOF_TOKEN at end
    void  print_all_tokens();    // debugging dump (drives --test-lexer)

    bool hadError = false;       // set on a lexical error; the driver aborts before parsing
};
```

The lexer runs a small preprocessor pass before tokenizing. The optional `macros`
table is shared across files so `#define`s propagate through `import` and
multi-file builds; `filename` is exposed to the preprocessor as `__FILE__` and
labels the lexer's diagnostics. `importHook`, when given, is called for each
`import` on an active line, in textual order, so the driver can preprocess the
imported file at that point and macros follow C's `#include` order. The pass itself
is a free function in `lexer/preprocessor.h`:

```cpp
void preprocess(const std::string& src, std::map<std::string, Macro>& defines,
                std::string& result, const std::string& filename, bool& hadErr,
                const PPImportHook* importHook = nullptr);
```

A streaming lexer: call `next_token()` until it returns a `Token` of type
`TokenType::EOF_TOKEN`. Callers collect the tokens into a `std::vector<Token>` for
the parser (pushing the final EOF token themselves).

### Token / TokenType

```cpp
enum class TokenType { /* keywords, operators, delimiters, literals, special */ };

struct Token {
    TokenType   type;
    std::string value;
    int         line;
    int         column;

    Token(TokenType type = TokenType::UNKNOWN, const std::string& value = "",
          int line = 0, int column = 0);
};

std::string tokenTypeToString(TokenType type);   // free function
```

`TokenType` enumerates every category the lexer recognizes: keywords (`INT`,
`RETURN`, `ASYNC`, `AWAIT`, `MATCH`, `ENUM`, `INTRINSIC`, …), operators (`EQ`,
`EQEQ`, `LSHIFT`, …), delimiters (`LBRACE`, `LBRACKET`, `ARROW`, `RANGE`, …),
literals (`INT_LIT`, `FLOAT_LIT`, `STRING_LIT`, `CHAR_LIT`, `IDENT`), and the
specials `PRAGMA`, `EOF_TOKEN`, `UNKNOWN`. Every `Token` carries its source
location (1-based line and column).

A `Macro` is the preprocessor's record of a `#define` (object-like or
function-like) with its parameter names and body text.

---

## Parser

Header: `parser/parser.h`

```cpp
class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens);

    std::shared_ptr<Program> parse();   // returns nullptr if any declaration failed

    bool hadError = false;              // set when a declaration/import failed to parse

    std::string                basedir;             // dir of the current file (relative imports)
    std::string                filename;            // current file; labels diagnostics
    std::string                stdlibPath;          // stdlib root (<module> imports)
    std::set<std::string>*     importedFiles = nullptr;   // shared dedup set (canonical paths)
    std::map<std::string,Macro>* macros = nullptr;        // shared macro table
    ImportCache*               importCache = nullptr;     // shared import prescan (preprocessed imports)
    std::set<std::string>*     sharedTypeNames = nullptr; // type names across all files

    static std::string canonicalPath(const std::string& path);   // importedFiles key
};
```

A hand-written recursive-descent parser that builds the AST. `parse()` consumes
the token vector and returns a `std::shared_ptr<Program>`; on a parse failure it
prints diagnostics to stderr, sets `hadError`, and returns `nullptr`, so callers
fail loudly rather than silently dropping code.

The public fields wire up multi-file compilation: `basedir` and `stdlibPath`
resolve `import "file.esk"` and `import <module>` respectively; `importedFiles`,
`macros`, and `sharedTypeNames` are shared with the sub-parsers spawned for
imported files so deduplication, macros, and declared type names propagate across
the whole compilation. `importedFiles` holds canonical paths (`canonicalPath`:
absolute, with `.`, `..` and symlinks resolved), and the driver seeds it with each
root input so a file that imports its importer is parsed once. The libraries named
by `#pragma link("name")` in the file and its imports end up in
`Program::linkLibs`, deduplicated in first-seen order. Nesting deeper than 100000
levels is a parse error (`nesting too deep`), and so is a type nested deeper than 1000
levels (`type nesting too deep`).

---

## Type Checker

Header: `sema/type_checker.h`

```cpp
class TypeChecker : public ASTVisitor {
public:
    TypeChecker();

    bool check(Program* program);          // entry point; returns false if errors were found

    std::string getExpressionType(Expr* expr);

    const std::map<Expr*, std::string>& expressionTypeMap() const;   // resolved type per expression
    const std::map<std::string, std::pair<std::string, std::vector<std::string>>>&
        instanceArgsMap() const;           // generic instances: mangled name -> template + args

    bool warnAll   = false;                // -Wall: lint warnings
    bool warnExtra = false;                // -Wextra: signed/unsigned mismatch, etc.

    // LSP / tooling
    std::string sourceFile = "unknown";    // file name for diagnostics
    std::string targetTriple;              // --target (empty = host): sizeof folds to its layout
    std::string getTypeAtPosition(int line, int col) const;   // --hover-at
    std::string getDefinitionAt(int line, int col) const;     // --definition-at
};
```

`check()` takes a raw `Program*` (it does not own the AST) and runs a two-pass
semantic analysis: a first pass registers top-level declarations (functions,
structs, templates, interfaces, enums, type aliases) so order is irrelevant, and
a second pass type-checks bodies. It returns `false` if any error was reported,
and accumulates errors with `file:line:col: message` locations. `warnAll` and
`warnExtra` enable lint diagnostics.

The tooling interface backs the editor integration: after a successful `check()`,
`getTypeAtPosition(line, col)` returns the inferred type of the expression (or
declared name) under a 1-based cursor, and `getDefinitionAt(line, col)` returns
the definition location of the symbol there. `getExpressionType(Expr*)` returns
the cached type of an already-visited expression node, and `expressionTypeMap()`
exposes the whole table (the async transform and codegen consume it).

As an `ASTVisitor`, the type checker overrides a `visit` method for every node
kind (declarations, statements, expressions); these are the traversal mechanism
and are not normally called directly.

---

## Async Transform

Header: `sema/async_transform.h`

```cpp
class AsyncTransform {
public:
    using InstanceArgs = std::map<std::string, std::pair<std::string, std::vector<std::string>>>;
    AsyncTransform(const std::map<Expr*, std::string>* exprTypes = nullptr,
                   const InstanceArgs* instanceArgs = nullptr);
    void run(Program* program);
};
```

Lowers `async` functions and `await` into a resumable state machine. It runs after type
checking and before code generation: each async function is replaced by a frame
struct, a `__<name>_resume` function (an if-chain over the resume state), and a
constructor that returns `*Future<T>`. Awaits are lowered across all control flow
(`if`/`else`, `while`, `do`/`while`, C-style `for`, `switch`, `match`, `for`-`in`
and `try`/`catch`), including `break`/`continue` and early `return`, and an await
may appear anywhere in an expression. The constructor takes the type checker's
`expressionTypeMap()` and `instanceArgsMap()`, which the lowering uses to type the
temporaries and frame fields it creates. The output is ordinary Eskiu AST that
normal code generation handles, so the pass mutates the `Program` in place and
returns nothing; a lowering error is thrown as `std::runtime_error` with a
`file:line:col` location.

---

## Code Generation

Header: `codegen/codegen.h`

```cpp
class CodeGen : public ASTVisitor {
public:
    CodeGen();
    ~CodeGen();

    llvm::Module* generateCode(std::shared_ptr<Program> program);  // null on failure
    llvm::Module* getModule() const;                               // non-owning

    std::string targetTriple;     // target triple override (empty = native)
    std::string targetCPU;        // --mcpu (empty = the target baseline: apple-m1 on arm64 Apple, else generic)
    std::string targetFeatures;   // --mattr feature string (LLVM -mattr syntax, e.g. "+vfp2")
    std::string relocModel;       // --reloc: "static" | "dynamic-no-pic" | "" ("pic", default)
    bool freestanding = false;    // alloc/free → esk_alloc/esk_free instead of calloc/free
    bool asan  = false;           // AddressSanitizer instrumentation
    bool ubsan = false;           // trapping bounds checks
    unsigned safeChecks = 0;      // --safe[=LIST]: SafeBounds | SafeOverflow | SafeNull
    unsigned optLevel = 0;        // -O level; 1..3 run the LLVM middle-end (0 = none)

    const std::map<Expr*, std::string>* resolvedExprTypes = nullptr;  // the type checker's table
    const std::map<std::string, std::pair<std::string, std::vector<std::string>>>*
        semaInstanceArgs = nullptr;                                    // its generic instances

    void printIR() const;                            // print IR to stdout (--test-codegen)
    void optimizeModule();                           // run the middle-end pipeline at optLevel
    bool emitObjectFile(const std::string& filename); // write a native object file
};
```

`generateCode()` walks the AST via the visitor pattern and emits LLVM IR through
an `IRBuilder`, returning the populated `llvm::Module*` (owned by the `CodeGen`
instance; `nullptr` on failure). `getModule()` returns the same module
non-owningly.

Configure the run before calling `generateCode()`:

- `targetTriple`: set to cross-compile for a given triple; empty targets the host.
  A hard-float ARM triple (one ending in `hf`, e.g. `armv6k-none-eabihf`) selects the
  hard-float ABI so the object carries the `Tag_ABI_VFP_args` build attribute needed to
  link against hard-float libraries. For a non-hosted triple (OS `none`) the driver
  (`seedPredefinedMacros`) predefines no OS macro.
- `targetCPU` / `targetFeatures`: `--mcpu` / `--mattr` overrides, forwarded to the
  `TargetMachine` (e.g. `mpcore` + `+vfp2` for the 3DS's ARM11). Empty CPU means
  the target baseline for native builds too, as in clang: `apple-m1` on arm64 Apple targets, `generic` elsewhere (pass `--mcpu` to tune for a CPU).
- `relocModel`: `--reloc` selects the relocation model. `static` (or `dynamic-no-pic`)
  instead of the default PIC; the 3DS `.3dsx` loader applies static relocations and has
  no dynamic loader to populate a GOT.
- `freestanding`: records `--freestanding`. The allocator routing itself happens in
  `<mem>`, which targets the user-supplied `esk_alloc`/`esk_free` when
  `__ESKIU_FREESTANDING__` is defined (`seedPredefinedMacros` defines it).
- `resolvedExprTypes` / `semaInstanceArgs`: point at the `expressionTypeMap()` and
  `instanceArgsMap()` of the type checker run on the transformed AST. Codegen reads
  expression types from this table instead of re-deriving them.
- `asan` / `ubsan`: apply the corresponding LLVM instrumentation pass to the
  module before object emission.
- `safe`: inject a bounds check on every slice and fixed-array index; an
  out-of-range access traps (`@llvm.trap`) instead of reading or writing past the
  end. Off by default, so release builds are unaffected.
- `optLevel` / `optimizeModule()`: `-O1`..`-O3` run the LLVM middle-end (mem2reg, SROA, inlining, GVN) over the module before object emission; `0` skips it (the default, naive IR straight to the backend).

`printIR()` prints the module's IR to stdout (valid before the module's ownership
is transferred away). `emitObjectFile(filename)` lowers the module to a native
object file for `targetTriple` (or the host) and writes it to `filename`,
returning `true` on success.

---

## Driver Support

Header: `main_support.h` (the `cl::opt`-free half of the driver, in `main_support.cpp`)

```cpp
std::shared_ptr<Program> loadProgram(const std::vector<std::string>& inputs, const std::string& triple,
                                     bool freestanding,
                                     std::map<std::string, Macro>* macrosOut = nullptr,
                                     bool* lexFailed = nullptr);
void seedPredefinedMacros(std::map<std::string, Macro>& macros, const std::string& triple,
                          bool freestanding);
std::vector<std::string> implicitLinkLibs(const std::map<std::string, Macro>& macros,
                                          bool usesExceptions, bool usesThreads);
bool linkExecutable(const std::string& obj, const std::string& out,
                    const std::vector<std::string>& libs,
                    const std::vector<std::string>& paths,
                    const std::vector<std::string>& extra,
                    bool sanitized = false);
int runExecutable(const std::string& exe, const std::vector<std::string>& progArgs);
```

`loadProgram` reads, lexes and parses the input files the way a build does (the macros
`seedPredefinedMacros` puts in for `triple`, `__FILE__`, imports resolved against
the stdlib root, declarations of all inputs merged into one `Program`) and returns
`nullptr` after printing a diagnostic; `macrosOut` receives the final macro table. The
build, the `--test-*`, `--hover-at` and `--definition-at` modes all use it. `implicitLinkLibs` returns the
runtime libraries a program implies on the target the macros describe (`c++` or
`stdc++` for exceptions, `pthread` for threads); `linkExecutable` invokes the C
driver (`findCDriver`: `$CC`, then `cc`/`clang`/`gcc`) with `-l` libraries, `-L`
paths and extra arguments. `runExecutable` backs `eskiuc run` and returns the
program's status, or `128+N` when signal `N` killed it.

---

## Pipeline Example

The passes compose as the driver (`main.cpp`) uses them. Steps 1 and 2 are what
`loadProgram` does for you (plus macro seeding and import handling):

```cpp
// 1. Lex
Lexer lexer(source, &macros, filename);
std::vector<Token> tokens;
for (Token t = lexer.next_token(); ; t = lexer.next_token()) {
    tokens.push_back(t);
    if (t.type == TokenType::EOF_TOKEN) break;
}
if (lexer.hadError) return 1;

// 2. Parse
Parser parser(tokens);
parser.filename = filename;
parser.basedir = ...; parser.stdlibPath = ...;
std::shared_ptr<Program> program = parser.parse();
if (!program) return 1;   // parse failure

// 3. Type-check
TypeChecker checker;
checker.sourceFile = filename;
if (!checker.check(program.get())) return 1;   // raw pointer; checker does not own the AST

// 4. Lower async → state machine (may throw std::runtime_error)
AsyncTransform(&checker.expressionTypeMap(), &checker.instanceArgsMap()).run(program.get());

// 4b. Re-run the type checker on the transformed AST: codegen consumes its
//     resolved per-expression types (the type checker is the single resolver).
TypeChecker postCheck;
postCheck.sourceFile = filename;
if (!postCheck.check(program.get())) return 1;   // an internal error

// 5. Code-generate
CodeGen codegen;
codegen.resolvedExprTypes = &postCheck.expressionTypeMap();
codegen.semaInstanceArgs  = &postCheck.instanceArgsMap();
codegen.targetTriple = ...;          // optional
codegen.freestanding = freestanding; // optional
if (!codegen.generateCode(program)) return 1;   // shared_ptr
codegen.emitObjectFile("out.o");     // or codegen.printIR();
```

Note the ownership boundary: the parser produces a `std::shared_ptr<Program>`;
`TypeChecker::check` and `AsyncTransform::run` take a raw `Program*` (they do not
own it); `CodeGen::generateCode` takes the `shared_ptr`.
