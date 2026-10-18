# Glossary

Reference glossary of terms used in Eskiu documentation and compiler source code.

## A

**Abstract Syntax Tree (AST)**
Tree-structured representation of a program's syntactic form after parsing. Each node corresponds to a language construct such as a function declaration, binary expression, or loop. The type checker and code generator both traverse the AST using the visitor pattern. See also: ASTVisitor, parser.

**algebraic data type (ADT)**
A type formed as a tagged choice between several variants, some of which may carry payload data, also called a sum type or tagged union. In Eskiu an `enum` with one or more payload-bearing variants is an ADT, laid out as `{ tag, payload }` and destructured with an exhaustive `match`. See also: enum, tagged union, match.

**alloca**
LLVM IR instruction that allocates space on the stack frame of the current function and returns a pointer to it. Every local variable in Eskiu compiles to an `alloca` in the function's entry block, followed by `store`/`load` instructions. Stack allocations are automatically reclaimed when the function returns.

**ALPN (Application-Layer Protocol Negotiation)**
A TLS handshake extension by which the client offers a list of protocols and the server selects one. The `<tls>` module installs an ALPN callback that selects `"h2"`, the way browsers negotiate HTTP/2 over TLS. See also: HTTP/2, TLS.

**async**
A function modifier (`async int f(...)`) marking a function that may suspend at `await` points. A call to an async function does not run it to completion; it yields a `*Future<T>` handle to the eventual result. The compiler lowers each async function into a resumable state-machine coroutine. See also: await, Future, coroutine.

**ASTVisitor**
Abstract base class that defines a `visit` method for each AST node type. Concrete subclasses (e.g., `TypeChecker`, `CodeGen`) implement these methods to traverse the tree without embedding logic inside the node classes themselves. See also: visitor pattern.

**atomic**
An operation on a shared memory cell that completes indivisibly with respect to other threads. The `<atomic>` module exposes `atomic_load`/`atomic_store`/`atomic_swap`/`atomic_cas` on an `int` cell, declared `intrinsic` and lowered to LLVM atomics with fixed acquire/release ordering. See also: intrinsic, executor.

**await**
An expression (`await E`) that suspends the enclosing `async` function until the future `E` completes, then evaluates to its result. `E` must have type `*Future<T>` and `await E` has type `T`. It is legal only inside an `async` function. See also: async, Future, coroutine.

## B

**basic block**
Maximal sequence of LLVM IR instructions with no branches except at the end. Every function body is a graph of basic blocks. Control-flow constructs (if, while, for) cause the code generator to create new basic blocks and wire them with branch instructions. See also: IRBuilder.

**binary operator**
An operator that takes exactly two operands. Eskiu supports arithmetic (`+`, `-`, `*`, `/`, `%`), bitwise and shift (`&`, `|`, `^`, `<<`, `>>`), comparison (`==`, `!=`, `<`, `<=`, `>`, `>=`), and logical (`&&`, `||`) binary operators. Precedence rules determine evaluation order when operators appear without parentheses. See also: unary operator, precedence.

**bitfield**
A struct integer field declared with a bit width (`uint32 mode : 3;`), occupying only that many bits. Bitfields are laid out the way the target's C compiler lays them out (SysV/AAPCS rules on Linux, macOS and bare-metal ARM, MS rules on Windows); reads mask and shift the field out (signed fields sign-extend) and writes are read-modify-write. The address of a bitfield cannot be taken. See also: packed struct, struct.

**BlockItem**
Union type used internally in the parser and AST to represent a single item inside a block statement: either a declaration (VarDecl, StructDecl) or an executable statement. Storing both as `BlockItem` allows the parser to handle declaration-in-block uniformly. See also: BlockStmt, declaration, statement.

**BlockStmt**
AST node representing a `{ ... }` block. Contains an ordered list of `BlockItem` entries and introduces a new nested scope. Functions, if-branches, loops, and standalone braces all produce a `BlockStmt`. See also: scope, BlockItem.

**bounded generic / type-parameter constraint**
A type parameter restricted to types that satisfy one or more interfaces, written `<T: Iface>` for a single bound or `<T: A + B>` for several. The constraint is checked at instantiation: every concrete type substituted for `T` must satisfy each named interface. A primitive type can satisfy a constraint via a free function that implements the required method set. Added in v0.2.2 (multi-bound `+`), with primitive satisfaction in v0.2.3. See also: interface, template / generic, monomorphization.

**bootstrap fixpoint**
The point at which a self-hosted compiler reproduces its own output. Eskiu's bootstrap builds the Eskiu-written compiler three times: the C++ `eskiuc` builds it (cc0), cc0 builds it (cc1), and cc1 builds it (cc2). cc1 and cc2 emit byte-identical IR for the compiler's own source, which proves the self-hosted compiler is a fixpoint. Reached in v0.3.0. See also: self-hosting.

## C

**channel**
An async message queue between tasks (`Chan<T>` in `<channel>`). `Chan_recv` returns a `*Future<T>` that completes with the next item: immediately if one is buffered, otherwise when a `Chan_send` hands a value off to the parked receiver. See also: Future, await.

**closure**
A function value that captures variables from its enclosing scope. Represented as a two-word fat pointer `{fn_ptr, env_ptr}`: a non-capturing closure has a null environment, while a capturing one packages its captured variables into an environment struct. The type annotation is `fn(T,...)->R` in both cases. See also: escaping, fat pointer, lambda.

**codegen**
Short form of "code generation." Refers to the code-generation pass of the Eskiu compiler, implemented across `codegen/codegen_*.cpp` (module, type, scope, decl, stmt, expr, call, closure, adt, cabi), which walks the AST and emits LLVM IR instructions via `IRBuilder`. Also used informally to describe any pass that produces output code. See also: IRBuilder, LLVM IR.

**coroutine**
A function whose execution can suspend and later resume. Each Eskiu `async` function is lowered to a coroutine: a frame struct holding its live state plus a resume function that advances an explicit state machine across `await` points. See also: async, await, Future.

## D

**declaration**
A language construct that introduces a new named entity into the current scope. Eskiu declaration forms are: `FunctionDecl`, `VarDecl`, `StructDecl`, `UnionDecl`, `EnumDecl`, `InterfaceDecl`, `TypeAliasDecl`, `IntrinsicDecl`, and `ExternDecl`. Declarations are distinct from statements in that they bind a name; statements execute logic. See also: statement, scope.

**defer / errdefer**
Statements that schedule cleanup to run when the enclosing block is left. `defer stmt;` runs on every exit path (fall-through, `return`, `break`, `continue`, and `?`-propagation), in LIFO order; `errdefer` runs only on the error exit (a propagated `?`), for undoing partial work when a later step fails. Both are lowered by a codegen cleanup stack rather than desugared, so the cleanup rides every branch out of the scope. See also: Result.

## E

**enum**
A declaration of named constants. A plain `enum` defines integer constants (the type behaves as `int`); an `enum` with one or more payload-bearing variants is an algebraic data type laid out as `{ tag, payload }` and destructured with `match`. Enums may be generic (e.g. `Option<T>`) and are monomorphized per instantiation. See also: algebraic data type, match, tagged union.

**escaping**
A parameter qualifier (`escaping fn(int)->void cb`) marking a function-pointer parameter that the callee retains beyond the call: by storing, returning, or forwarding it. An escaping closure's environment is heap-allocated (released with `free_closure`) so it outlives the creating frame; a non-escaping closure keeps its environment on the stack. Using a non-`escaping` parameter beyond a direct call is a compile error. See also: closure, fat pointer.

**event loop / reactor**
A single thread that watches many file descriptors and dispatches a callback when one becomes ready, via kqueue (macOS), epoll (Linux) or `WSAPoll` (Windows). Implemented as `EventLoop` in `<eventloop>`, it is the readiness reactor underpinning async I/O, the HTTP stack, and the timer wheel. See also: executor, Future, async.

**executor**
A thread that owns an event loop plus a thread-safe ready-queue of wakers (`Executor` in `<executor>`). Completion may occur on any thread; `Executor_schedule` enqueues a waker and wakes the loop through a self-pipe so the waker (a coroutine resume) always runs on the executor's own thread. See also: event loop / reactor, coroutine, Future.

**expression**
A syntactic form that evaluates to a value and has a type. Examples: `3 + 4`, `add(5, 2)`, `point.x`, `*ptr`. Expressions form the leaves and internal nodes of most AST subtrees. See also: lvalue, rvalue, statement.

**ExternDecl**
AST node representing a declaration of a function whose implementation lives in an external C library. Syntax: `extern int printf(string fmt, ...);`. The type checker records the function signature; the code generator emits an LLVM `declare` instruction so the linker can resolve it. A struct passed or returned by value follows the target's C calling convention, and a parameter of fn type is a C function pointer (the call passes a top-level function by name or `null`). See also: variadic, declaration.

## F

**fat pointer**
A two-word value carrying a data pointer alongside a second word. Eskiu uses fat pointers in three places: a closure / function-pointer value is `{fn_ptr, env_ptr}`, an interface value is `{data_ptr, vtable_ptr}`, and a slice is `{data_ptr, length}`. The representation is transparent to user code. See also: closure, interface, vtable.

**flow control**
In HTTP/2, the credit-based mechanism that bounds how much DATA a sender may transmit before the receiver grants more window via WINDOW_UPDATE frames, applied per stream and per connection. The `<http2>` `H2Stream` tracks the send window and the server emits DATA in flow-controlled, bounded frames. See also: HTTP/2, stream multiplexing.

**FunctionDecl**
AST node representing a function definition, including its name, parameter list, return type, and body (`BlockStmt`). During type checking, parameters are pushed into a new scope and the body is validated against the declared return type. During code generation, a new LLVM `Function` object is created and populated. See also: declaration, scope.

**Future**
A handle to a value that may not exist yet: the result of an `async` function call or a leaf primitive. `Future<T>` (in `<future>`) is a fixed-layout struct `{ state, waker, on_drop, value }`: `state` advances through PENDING/WAITING/READY/CANCELLED, `waker` resumes the awaiter on completion, and `on_drop` releases resources on cancellation. A pending future is cancelled with `Future_drop`. See also: async, await, executor.

**fuzzer / differential oracle**
A generative test harness (`tests/fuzz/eskiu_fuzz.py`) that synthesizes random valid Eskiu programs and compiles each at `-O0` and `-O2`, then compares their output. A mismatch is a differential failure: it catches miscompiles that pass the LLVM verifier yet behave differently across optimization levels. See also: codegen, LLVM IR.

## G

**GEP (getelementptr)**
LLVM IR instruction that computes the address of a sub-element of an aggregate type (array or struct) without loading data. Used by the Eskiu code generator to access struct fields and array elements. GEP does pointer arithmetic at the IR level and produces a typed pointer to the target element.

## H

**HPACK**
The HTTP/2 header-compression format (RFC 7541). It combines a static table of common header fields, a per-connection dynamic table the peers grow as they go, and a compact wire coding (prefix integers and optionally Huffman-coded string literals). Implemented in `<hpack>`. See also: HTTP/2, ALPN.

**HTTP/2 frame**
The unit of the HTTP/2 wire protocol (RFC 7540): a 9-byte binary header (length, type, flags, stream id) followed by a payload. Frame types include DATA, HEADERS, SETTINGS, WINDOW_UPDATE, PING, and GOAWAY. Many frames sharing one connection are interleaved across streams. The `<http2>` module provides the frame-header codec and frame I/O. See also: stream multiplexing, flow control, HPACK.

## I

**identifier**
A name chosen by the programmer to label a variable, function, struct, or parameter. In Eskiu, identifiers must begin with a letter or underscore and may contain letters, digits, and underscores. The lexer emits an `IDENT` token; the parser stores the raw string in `IdentExpr` or declaration nodes.

**integer promotion**
The C rule that converts an operand narrower than `int` (`bool`, `char`, `int8`, `int16`, `uint8`, `uint16`) to `int` before an arithmetic, bitwise, shift or comparison operator is applied. Eskiu follows it since v0.9.2, so `(uint8)200 + (uint8)100` is `300`. See also: type coercion.

**interface**
A structurally typed contract defined by a method set (`interface Drawable { void draw(); }`). A concrete type satisfies an interface implicitly whenever it provides all the named methods with matching signatures: no explicit `implements` declaration is required. An interface value is a fat pointer `{data_ptr, vtable_ptr}` made from a pointer to the struct (`&x`); it can be stored in a local or field, passed and returned, and method calls dispatch dynamically through the vtable. Passing a struct by value where an interface is expected is an error. See also: vtable, fat pointer, bounded generic / type-parameter constraint.

**intrinsic**
A function declared with the `intrinsic` qualifier whose calls the compiler lowers to inline IR rather than an ordinary call. Used for operations that must compile directly to specific instructions, such as the `<atomic>` cell operations. See also: atomic, declaration.

**IRBuilder**
LLVM C++ API class (`llvm::IRBuilder<>`) that provides a cursor-based interface for inserting IR instructions into a basic block. Eskiu's code generator holds a single `IRBuilder` instance and repositions it as it enters new blocks. See also: basic block, codegen.

## L

**lambda / closure**
An anonymous function expression, written `int(int n) { return n * n; }` and typed `fn(T,...)->R`. A lambda that references variables from its enclosing scope is a closure; escape analysis decides whether its captured environment lives on the stack (non-escaping) or the heap (escaping). At runtime both compile to a two-word fat pointer `{fn_ptr, env_ptr}`. See also: closure, escaping, fat pointer.

**lexer**
The first phase of the Eskiu compiler, implemented in `lexer/`. Reads the raw source text character by character and produces a flat sequence of tokens annotated with type, value, line, and column. The lexer handles whitespace, comments, string literals, numeric literals, keywords, and punctuation. See also: token, TokenType, parser.

**literal**
A compile-time constant value written directly in source code. Eskiu literal kinds: integer literals (`42`), floating-point literals (`3.14`), string literals (`"hello"`), boolean literals (`true`, `false`), and character literals (`'a'`). Represented in the AST as `LiteralExpr`. See also: expression.

**LLVM IR**
The text and in-memory intermediate representation used by the LLVM compiler infrastructure. Eskiu compiles to LLVM IR, which is then lowered to machine code by LLVM's optimization and backend passes. IR is strongly typed, uses Static Single Assignment (SSA) form, and is human-readable in `.ll` files. See also: codegen, basic block, IRBuilder.

**lvalue**
An expression that refers to a storage location and can appear on the left side of an assignment. In Eskiu, variable names and pointer dereferences (`*ptr`) are lvalues. The code generator resolves lvalues to their `alloca` address (emitting a GEP for struct fields), then uses `store` to write the value. See also: rvalue, alloca.

## M

**match**
A control construct that destructures an algebraic-enum value by variant, binding each variant's payload fields in its arm. A `match` must be exhaustive (every variant has an arm or a `_` default catches the rest) and no variant may appear twice; the type checker enforces both. See also: algebraic data type, enum, tagged union.

**module**
The top-level LLVM IR container (`llvm::Module`) that holds all function definitions, global variables, and external declarations produced for a single compilation unit. The code generator creates one module per compilation: all input files and their imports are merged into one program first. The module is printed as LLVM IR text when `--test-codegen` is passed.

**monomorphization**
The compilation strategy by which a generic definition is specialized into a separate concrete copy for each distinct set of type arguments it is instantiated with. Eskiu templates and generic enums are monomorphic: instead of boxing or runtime dispatch, the compiler emits one stamped-out version per instantiation, each with a mangled name. See also: template / generic, monomorphic.

**must_use**
A function qualifier (`must_use *uint8 grab() { ... }`) that makes discarding the call's result a compile error, catching a leaked allocation or an ignored status at compile time. The stdlib's `alloc` is `must_use`, so a bare `alloc<T>(n);` is rejected. See also: Result.

## N

**narrowing**
The type checker's knowledge that a `?*T` variable is non-null at a given point, established by a null check: `if (x != null)`, an early exit such as `if (x == null) { return; }`, a loop condition, the left side of `&&`/`||`, or a ternary condition. Narrowing is flow-sensitive: an assignment to the variable ends it, and a shadowing declaration of the same name is not narrowed. While narrowed, the variable may be dereferenced and passed where a `*T` is expected. See also: nullable pointer.

**nullable pointer (`?*T`)**
A checked pointer type that may hold `null`. Unlike a bare `*T` (C-nullable but unchecked), a `?*T` cannot be dereferenced, indexed, or member-accessed until it is proven non-null; a null check such as `if (x != null) { ... }` narrows it to non-null. A `*T` widens to `?*T` implicitly; the reverse needs a check. It lowers to a bare pointer, so the safety is entirely at compile time. See also: pointer, narrowing.

## O

**opaque pointer**
LLVM pointer type in LLVM 15+ (`ptr`) that carries no element-type information; replaced typed pointers (`i32*`, `i8*`, etc.). Eskiu targets opaque-pointer mode and uses GEP with explicit type hints to navigate aggregate structures. See also: GEP, pointer type.

## P

**packed struct**
A struct laid out with no inter-field padding, so fields sit back-to-back. Marked with the `packed` qualifier or `#pragma pack(1)`; `#pragma pack(N)` for `N > 1` instead caps each field's alignment at `N`. Used to match an exact on-the-wire or on-disk byte layout, or a C struct declared `__attribute__((packed))`. The chosen layout is reflected by `sizeof` and every field access. See also: bitfield, struct.

**parser**
The phase of the Eskiu compiler after the lexer, implemented in `parser/`. Consumes the token stream produced by the lexer and builds the AST using recursive descent. Handles declarations, statements, and expressions with explicit precedence climbing. See also: recursive descent, AST, precedence.

**phase**
A stage of the compiler pipeline: preprocessing and lexing, parsing, type checking, the async transform, and code generation. Each phase is a separate pass over the program (see `docs/dev/architecture.md`). See also: codegen, type checker, semantic analysis.

**pointer type**
A type that holds the memory address of a value of another type. Eskiu accepts both leading-star notation (`*T`) and trailing-star notation (`T*`) in source code; both are normalized to the same internal representation. Pointer arithmetic and dereferencing are supported; pointer safety is the programmer's responsibility. See also: opaque pointer, GEP, lvalue.

**precedence**
The binding strength of an operator relative to others. Higher-precedence operators bind their operands before lower-precedence ones. Eskiu follows C operator precedence: multiplicative (`*`, `/`, `%`) before additive (`+`, `-`), which is before relational, then equality, then logical-and, then logical-or. Bitwise and shift operators sit between them as in C. The parser implements precedence by precedence climbing (`parseBinary`), with assignment and the ternary in their own recursive-descent functions. See also: binary operator, parser.

**program**
A complete Eskiu source file (`.esk`) that defines or declares all entities needed for compilation. At the AST level a program is a `Program` node containing a list of top-level declarations. See also: declaration, module.

## R

**recursive descent**
Parsing strategy in which each grammar rule is implemented as a mutually recursive function. Eskiu's parser is a hand-written recursive-descent parser; `parseDeclaration`, `parseStatement`, and `parseExpression` call each other according to the grammar's structure. See also: parser.

**rvalue**
An expression whose value can be read but that does not itself designate a storage location. Literals, arithmetic results, and function-call results are rvalues in Eskiu. The code generator evaluates rvalues with `load` instructions (when reading a variable) or directly as IR values (for computed results). See also: lvalue.

## S

**safe mode (`--safe`)**
An opt-in build mode that inserts run-time checks: a bounds check on every array and slice index, an overflow check on signed `+ - *` over `int`/`int64`, and a null check before a pointer dereference; a violation traps (`@llvm.trap`) instead of continuing. `--safe=LIST` picks a subset (`bounds`, `overflow`, `null`). Off by default, so release builds pay nothing. See also: slice, sanitizer.

**scope**
The region of source code in which a declared name is visible. Eskiu uses lexical (block) scoping: each `BlockStmt` introduces a new scope. The symbol table supports nested scopes via a scope stack; inner scopes can shadow outer ones. Names go out of scope when their enclosing block ends. See also: symbol table, BlockStmt.

**self-hosting**
A compiler written in the language it compiles. The whole Eskiu compiler (lexer, preprocessor, parser, type checker, and code generator) is reimplemented in Eskiu under `selfhost/`, validated for parity against the production C++ `eskiuc` and against itself through a 3-stage bootstrap fixpoint. Shipped in v0.3.0; the code generator is feature-complete against the C++ corpus. See also: bootstrap fixpoint, codegen.

**semantic analysis**
The compiler phase that validates program meaning beyond syntactic correctness. In Eskiu this is the type checker: it verifies type compatibility, resolves identifiers, checks struct-field existence, validates function call arities and types, and enforces return-type consistency. See also: type checker, scope.

**slice (`T[]`)**
A fat pointer (data pointer + length) that views a contiguous run of `T` without owning it. Built by slicing a fixed array with a half-open range (`a[lo..hi]`); it supports `s[i]`, `s.len`, and `for (x in s)`. Because the length travels with the slice, a function taking `T[]` needs no separate count argument. Lowers to `{ ptr, i64 }`. See also: fat pointer, array, safe mode.

**sret (structure return)**
The calling convention for returning a struct too large to fit in registers: the caller passes a hidden pointer to a result slot, the LLVM function itself returns `void`, and the body writes the result through that pointer. The code generator applies sret automatically to large aggregate return types. See also: codegen, struct.

**statement**
A language construct that performs an action but does not itself produce a value. Eskiu statement kinds include `BlockStmt`, `IfStmt`, `ForStmt`, `ForInStmt`, `WhileStmt`, `DoWhileStmt`, `SwitchStmt`, `MatchStmt`, `ReturnStmt`, `BreakStmt`, `ContinueStmt`, `TryStmt`, `DeferStmt`, and `ExprStmt`. Statements are sequenced inside `BlockStmt`. See also: expression, declaration.

**stream multiplexing**
In HTTP/2, the interleaving of many concurrent request/response exchanges (streams), each with its own id, over a single connection. The `<http2_server>` routes interleaved frames to per-stream slots, each completing when its END_STREAM arrives. See also: HTTP/2 frame, flow control.

**struct**
A composite type composed of named fields, each with its own type. Declared with `struct Name { ... }`. Fields are accessed via the `.` member operator. Methods can be defined on structs. The type checker validates field access; the code generator lays fields out sequentially in an LLVM struct type and uses GEP to address them. See also: StructDecl, GEP.

**StructDecl**
AST node representing a struct type definition, including the struct's name, its field list (name + type pairs), and any method declarations. Registered in the struct registry during semantic analysis so that `MemberExpr` nodes can look up field types. See also: struct, declaration.

**symbol table**
A data structure mapping identifier names to their resolved types and declarations within a scope. Eskiu's type checker maintains a stack of symbol-table frames, pushing a new frame on block entry and popping it on block exit. Used to resolve variable references and detect undeclared identifiers. See also: scope, identifier.

## T

**tagged union**
A value that holds one of several alternatives, distinguished by an integer tag stored alongside the payload: the runtime representation of an Eskiu algebraic-enum value (`{ tag, payload }`, with the payload area sized to the largest variant). Unlike a `union`, a tagged union records which alternative is active. See also: algebraic data type, enum, match, union.

**template / generic**
A definition parameterized over one or more type variables (`struct Vec<T> { ... }`, generic functions, and generic enums such as `Option<T>`). Generics in Eskiu are monomorphic: each instantiation is stamped out as a concrete copy with a name produced by `mangleTemplate`, rather than compiled once with type erasure. A type parameter may carry a constraint (`<T: Iface>`). See also: monomorphization, bounded generic / type-parameter constraint, enum.

**TLS**
Transport Layer Security, the encryption layer HTTP/2 typically runs over in browsers. The `<tls>` module wraps OpenSSL (libssl) by FFI and uses ALPN to negotiate `"h2"`, then runs the `<http2>` frame protocol over the encrypted stream. See also: ALPN, HTTP/2.

**token**
The smallest meaningful unit produced by the lexer. Each token carries a `TokenType`, its raw lexeme string, and a source location (line, column). Examples: keyword `int`, identifier `result`, punctuation `{`, integer literal `42`, string literal `"hello"`. See also: TokenType, lexer.

**TokenType**
Enumeration of all token categories recognized by the Eskiu lexer. Includes keywords (`INT`, `RETURN`, `IF`, `WHILE`, ...), literals (`INT_LIT`, `FLOAT_LIT`, `STRING_LIT`, `CHAR_LIT`), identifiers (`IDENT`), operators, punctuation, and `EOF_TOKEN`. Used in the parser's `check()`, `match()` and `consume()` helpers to drive grammar rules. See also: token, lexer.

**`ty::Type` IR**
The compiler's structured, in-memory representation of types (`sema/type.{h,cpp}`), replacing earlier reliance on raw type-name strings. Each `ty::Type` describes a type by kind (primitive, pointer, struct, interface, generic instantiation, function, and so on) and its components, so the type checker compares and resolves types by structure rather than by string matching. Introduced in v0.2.3; since v0.2.4 the type checker, working over it, is the single type resolver. See also: type checker, type unification, type normalization.

**type checker**
The compiler component that traverses the AST, infers or verifies the type of every expression, validates declarations, and reports semantic errors. It maintains the symbol table and struct registry. The implementation is split across `sema/type_checker.cpp` and `sema/typecheck_{decl,stmt,expr,type}.cpp`, and it operates over a structured `ty::Type` IR (`sema/type.{h,cpp}`); since v0.2.4 the type checker is the single resolver via type unification. See also: semantic analysis, type inference, symbol table, ty::Type IR, type unification.

**type coercion**
Implicit or explicit conversion of a value from one type to another. Eskiu requires explicit casts for most conversions (e.g., `(int)myFloat`). The type checker identifies cases where implicit promotion is safe (e.g., widening integer types in expressions) and emits the appropriate LLVM extension or truncation instructions. See also: type inference, CastExpr.

**type inference**
The compiler's ability to deduce the type of an expression or variable from context without an explicit annotation. Eskiu performs limited local type inference (for example, the right-hand side of a declaration can constrain the variable's type) but does not perform full Hindley-Milner inference. Explicit type annotations are generally required. See also: type checker, VarDecl.

**type normalization**
The process of canonicalizing type names to a single internal representation before comparison or storage. In Eskiu the type checker's `normalizeType` expands type aliases to their targets, tags struct and interface names (`Point` becomes `struct:Point`), instantiates and mangles generic types (`Result<int,string>` becomes `struct:Result_int_string`), and treats `*T` and `T*` as the same pointer type, so the type checker and code generator operate on a consistent set of type descriptors. See also: type checker, pointer type.

**type unification**
The process of reconciling two `ty::Type` values into a single consistent type: for example matching a call argument against a parameter, or inferring a generic type parameter from the types supplied at a call site. Introduced in v0.2.4, unification makes the type checker the single authority on type resolution, so code generation no longer re-derives types from name strings. See also: ty::Type IR, type checker, type inference.

## U

**unary operator**
An operator that takes a single operand. Eskiu unary operators: `-` (arithmetic negation), `!` (logical not), `~` (bitwise not), `*` (pointer dereference), `&` (address-of), represented in the AST as `UnaryExpr`, plus prefix and postfix `++`/`--` (`IncDecExpr`). See also: binary operator.

## V

**variadic**
A function that accepts a variable number of arguments after a fixed parameter list, indicated by `...` in its signature. Used primarily for C-interop functions such as `printf`. The type checker allows variadic calls to accept any arguments beyond the fixed parameters; LLVM emits a variadic function declaration. See also: ExternDecl.

**VarDecl**
AST node representing the declaration of a local or global variable. Stores the variable's name, declared type (may be inferred), and optional initializer expression. The type checker validates that the initializer type matches the declared type; the code generator emits an `alloca` and an optional `store`. Supports both C-style (`int x = 5;`) and `let`-style (`let x: int = 5;`) syntax. See also: alloca, declaration, type inference.

**visitor pattern**
Object-oriented design pattern in which an external object (the visitor) defines operations on the nodes of a data structure without modifying those nodes. Eskiu's AST nodes expose an `accept(ASTVisitor*)` method; the type checker and code generator each implement `ASTVisitor` to walk the tree. This separates compiler passes from AST node definitions. See also: ASTVisitor, AST.

**vtable**
A table of function pointers, one per method of an interface, shared by all values of a given concrete type. It is the second word of an interface fat pointer `{data_ptr, vtable_ptr}`: a method call loads the target function from the vtable at a fixed slot and invokes it with the data pointer as the receiver, giving dynamic dispatch. See also: interface, fat pointer.

---

- [architecture.md](../dev/architecture.md): Compiler architecture and pass pipeline
- [spec.md](spec.md): Full language specification
- [design.md](../dev/design.md): Rationale for key design choices
- [roadmap.md](../dev/roadmap.md): Roadmap
