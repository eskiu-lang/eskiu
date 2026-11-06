# Eskiu Grammar

A formal grammar for Eskiu, reflecting the actual recursive-descent parser
(`parser/`) and lexer (`lexer/lexer.cpp`). It is the authoritative
syntax reference; the prose in [spec.md](spec.md) explains semantics.

Notation (EBNF):

| Form | Meaning |
|------|---------|
| `x?`        | optional |
| `x*`        | zero or more |
| `x+`        | one or more |
| `x (',' x)*`| comma-separated list |
| `a \| b`    | alternation |
| `'tok'`     | literal token |
| `UPPER`     | a token class (terminal); see [Lexical](#lexical-structure) |
| `lower`     | a non-terminal |

The grammar is presented top-down: lexical structure, then the preprocessor,
then declarations, types, statements, and expressions (lowest precedence first).

---

## Lexical structure

Tokens are produced after the [preprocessor](#preprocessor) pass. Whitespace and
comments separate tokens and are otherwise insignificant.

```
IDENT      = [A-Za-z_] [A-Za-z0-9_]*
INT_LIT    = [1-9][0-9]*  |  '0' [0-7]*  |  '0x' [0-9A-Fa-f]+   // leading 0 = octal (C rule)
FLOAT_LIT  = [0-9]+ '.' [0-9]+ EXP?  |  [0-9]+ EXP
EXP        = ('e' | 'E') ('+' | '-')? [0-9]+
STRING_LIT = '"' ( escape | not('"') )* '"'        // adjacent literals concatenate: "a" "b" == "ab"
CHAR_LIT   = "'" ( escape | not("'") ) "'"
escape     = '\' ( 'n' | 't' | 'r' | 'f' | 'v' | 'a' | 'b' | '\' | '"' | "'" | '?'  // string and char share the set
                 | OCT OCT? OCT?                                         // raw byte, 1-3 octal digits (<= \377)
                 | 'x' HEX HEX? )                                        // raw byte, 1-2 hex digits; any other escape is an error
```

Comments: `// … end-of-line` and `/* … */` (block comments do not nest).

### Keywords (reserved)

```
let  const  volatile  static  async  await  escaping  must_use
int int8 int16 int32 int64  uint uint8 uint16 uint32 uint64
float double bool char string void
struct packed union interface enum fn
if else while do for in switch match case default break continue return
import extern intrinsic  sizeof alloc_with free_closure
thread_create thread_join  asm  try catch finally throw  defer errdefer  operator
null true false
```

`type` is **not** reserved: it is a contextual keyword recognized only in the
type-alias form `type NAME = …`.

### Operators and punctuation

```
+  -  *  /  %                          arithmetic
++  --                                 increment / decrement (prefix or postfix)
=  +=  -=  *=  /=  %=  &=  |=  ^=  <<=  >>=   assignment (compound forms desugar)
==  !=  <  >  <=  >=                   comparison
&&  ||  !                             logical
&  |  ^  ~  <<  >>                     bitwise
( ) { } [ ]  ;  ,  .                  delimiters
..   ...   :   ->   ?                  range, ellipsis, colon, arrow, error-propagation
```

---

## Preprocessor

A text pass run before tokenizing. Directives occupy a whole line; skipped or
directive lines become blank lines so source line numbers are preserved.

```
directive =
    '#define' IDENT macro-body
  | '#define' IDENT '(' (IDENT (',' IDENT)*)? ')' macro-body    // function-like
  | '#undef'  IDENT
  | '#ifdef'  IDENT  |  '#ifndef' IDENT  |  '#if' const-expr  |  '#elif' const-expr
  | '#else'  |  '#endif'
  | '#pragma' …                          // passed through to the compiler (pack, link)
  | '#error'  text                       // aborts compilation on an active branch
  | '#!' …                               // shebang on the first line: ignored
```

Predefined macros: `__FILE__`, `__LINE__`, a target-OS macro
(`__APPLE__`/`__linux__`/`_WIN32` and `_WIN64`), an architecture macro (`__aarch64__`/`__x86_64__`/`__arm__`),
and `__ESKIU_FREESTANDING__` under `--freestanding`.

---

## Program

```
program     = item*
item        = import | declaration

import      = 'import' STRING_LIT ';'           // relative path
            | 'import' '<' module-path '>' ';'  // stdlib module
```

---

## Declarations

```
declaration =
    function-decl | struct-decl | union-decl | interface-decl | enum-decl
  | type-alias  | extern-decl  | intrinsic-decl | var-decl

function-decl   = 'export'? ('async' | 'must_use')? type IDENT type-params? '(' param-list? ')' ( block | ';' )
                | type 'operator' overload-op '(' param-list ')' block   // operator overload
overload-op     = '+' | '-' | '*' | '/' | '%' | '==' | '!=' | '<' | '>' | '<=' | '>='
                | '&' | '|' | '^' | '<<' | '>>' | '!' | '~' | '[' ']'
extern-decl     = 'extern'    type IDENT '(' param-list? ')' ';'
                | 'extern' 'const'? type IDENT ';'                       // C global variable
intrinsic-decl  = 'intrinsic' type IDENT '(' param-list? ')' ';'

type-params     = '<' type-param (',' type-param)* '>'
type-param      = IDENT ( ':' IDENT ('+' IDENT)* )?   // optional interface constraint(s)

param-list      = param (',' param)* (',' '...')?  |  '...'
param           = 'escaping'? type IDENT

struct-decl     = 'packed'? 'struct' IDENT type-params? '{' struct-member* '}'
struct-member   = type IDENT (':' INT_LIT)? ';'                 // field, optional bitfield width
                | type ':' INT_LIT ';'                          // unnamed bitfield (width may be 0)
                | type IDENT '(' param-list? ')' block          // method

union-decl      = 'union' IDENT '{' ( type IDENT ';' | type ':' INT_LIT ';' )* '}'

interface-decl  = 'interface' IDENT '{' ( type IDENT '(' param-list? ')' ';' )* '}'

enum-decl       = 'enum' IDENT type-params? '{' enum-variant (',' enum-variant)* ','? '}'   // enum type-params are names only (no constraints)
enum-variant    = IDENT ( '(' type (',' type)* ')' )?           // ADT payload
                | IDENT ( '=' expr )?                            // classic, optional integer constant expression

type-alias      = 'type' IDENT '=' type ';'

var-decl        = qualifier* 'let' IDENT ':' type ( '=' expr )? ';'
                | qualifier* type IDENT ( '=' expr )? ';'
qualifier       = 'const' | 'volatile' | 'static'
```

`const` placement follows C: before a `let` it qualifies the binding; within a
type it qualifies the pointee (`const int*`) or pointer level (`int* const`).

`static` is a storage qualifier valid only on a **local** variable: the variable
has a single instance that persists across calls (like C). Its initializer must
be a compile-time constant. Applying `static` to a global is rejected (a global
already has static storage).

---

## Types

```
type        = '?'? 'const'? ptr* base suffix*      // leading '?' = checked nullable pointer `?*T`
suffix      = array | '*' 'const'?                   // arrays and trailing pointers, any order
base        = scalar-type
            | IDENT ( '<' type (',' type)* '>' )?   // named type or template instance
            | 'fn' '(' (type (',' type)*)? ')' '->' type   // function-pointer type
ptr         = '*' '?'?                               // leading-pointer (spec) spelling; `*?*T` (a `?`
                                                     // before another '*') points to a nullable pointer
array       = '[' const-expr? ']'                    // integer constant expression; empty = slice `T[]`

scalar-type = 'int' | 'int8' | 'int16' | 'int32' | 'int64'
            | 'uint' | 'uint8' | 'uint16' | 'uint32' | 'uint64'
            | 'float' | 'double' | 'bool' | 'char' | 'string' | 'void'
```

Pointers may be written either C-style (`int*`) or leading (`*int`); both are
equivalent. An array suffix binds tighter than a leading pointer, so `*T[N]` is an array
of N pointers (each element a `*T`). There is no pointer-to-array type (`T[N]*` does not
parse); use a `*T` to the first element. Suffixes chain for multidimensional arrays: `T[N][M]` is N
arrays of M (C order, leftmost bracket outermost). Empty brackets make a **slice**: `T[]`
is a fat pointer (data + length), constructed by slicing an array (`a[lo..hi]`). `va_list`
is a built-in named type used by variadics.

---

## Statements

```
statement =
    block | if-stmt | labeled-loop | while-stmt | for-stmt | switch-stmt | match-stmt
  | do-while-stmt  | return-stmt | break-stmt | continue-stmt | throw-stmt | try-stmt
  | defer-stmt | asm-stmt | thread-join-stmt | var-decl | expr-stmt

block         = '{' ( declaration | statement )* '}'
if-stmt       = 'if' '(' expr ')' statement ( 'else' statement )?
labeled-loop  = IDENT ':' ( while-stmt | do-while-stmt | for-stmt )   // names a loop for break/continue
while-stmt    = 'while' '(' expr ')' statement
do-while-stmt = 'do' statement 'while' '(' expr ')' ';'
for-stmt      = 'for' '(' ( var-decl | expr )? ';' expr? ';' expr? ')' statement
              | 'for' '(' IDENT 'in' expr ( '..' expr )? ')' statement   // iterate / half-open range
return-stmt   = 'return' expr? ';'
break-stmt    = 'break' IDENT? ';'      // IDENT = a labeled enclosing loop (default: innermost)
continue-stmt = 'continue' IDENT? ';'   // IDENT = a labeled enclosing loop (default: innermost)
throw-stmt    = 'throw' expr ';'
expr-stmt     = expr ';'

switch-stmt   = 'switch' '(' expr ')' '{' (switch-case | default-case)* '}'   // any order, at most one `default`
switch-case   = 'case' expr ':' ( declaration | statement )*   // the switch body is one scope
default-case  = 'default' ':' ( declaration | statement )*

match-stmt    = 'match' expr '{' match-arm+ '}'
match-arm     = IDENT ( '(' IDENT (',' IDENT)* ')' )? '->' statement   // variant + payload bindings
              | '_' '->' statement                                     // default

try-stmt      = 'try' block ( 'catch' '(' type IDENT ')' block )* ( 'finally' block )?
defer-stmt    = ( 'defer' | 'errdefer' ) statement   // block-exit cleanup, LIFO; errdefer runs only on the `?`-error path
asm-stmt      = 'asm' '(' STRING_LIT ( ':' asm-operands                 // outputs: "=r" / "+r" / "=m" (lvalues)
                              ( ':' asm-operands                  // inputs: "r" / "m" / "i"; x86 GCC "a" "Nd" or LLVM "{ax}" "N{dx}"
                              ( ':' STRING_LIT (',' STRING_LIT)* )? )? )? ')' ';'
asm-operands  = ( asm-operand (',' asm-operand)* )?
asm-operand   = STRING_LIT '(' expr ')'
thread-join-stmt = 'thread_join' '(' expr ')' ';'
```

`for (i in A..B)` desugars to a counted `for (T i = A; i < B; i = i + 1)`, where `T` is
the bounds' common integer type (see the spec's for-in section).

---

## Expressions

Precedence, lowest to highest. Each level is left-associative unless noted.

```
expr            = assignment
assignment      = ternary ( assign-op assignment )?              // right-assoc
assign-op       = '=' | '+=' | '-=' | '*=' | '/=' | '%='
                | '&=' | '|=' | '^=' | '<<=' | '>>='              // compound forms desugar
ternary         = logical-or ( '?' expr ':' ternary )?           // right-assoc
logical-or      = logical-and ( '||' logical-and )*
logical-and     = bitwise-or  ( '&&' bitwise-or  )*
bitwise-or      = bitwise-xor ( '|'  bitwise-xor )*
bitwise-xor     = bitwise-and ( '^'  bitwise-and )*
bitwise-and     = equality    ( '&'  equality    )*
equality        = comparison  ( ('==' | '!=') comparison )*
comparison      = shift       ( ('<' | '>' | '<=' | '>=') shift )*
shift           = additive    ( ('<<' | '>>') additive )*
additive        = multiplicative ( ('+' | '-') multiplicative )*
multiplicative  = unary       ( ('*' | '/' | '%') unary )*
unary           = ('!' | '-' | '+' | '&' | '*' | '~' | '++' | '--' | 'await') unary   // right-assoc
                | cast
cast            = '(' type ')' unary
                | postfix
postfix         = primary postfix-op*
postfix-op      = '(' arg-list? ')'          // call
                | '[' expr ']'               // index
                | '[' expr '..' expr ']'     // slice (half-open) → a `T[]` fat pointer
                | '.' (IDENT | keyword)      // member (a keyword is a name here: `j.int(5)`)
                | '?'                        // error propagation (Result)
                | '++' | '--'                // post-increment / decrement
arg-list        = expr (',' expr)*
```

`?` is overloaded: `cond ? a : b` is the conditional (ternary) operator, while a
postfix `expr?` (with no matching `:` ahead) is the Result error-propagation operator
(it returns early on the error variant). The parser disambiguates by scanning for a
same-level `:` after the `?`. Assignment is the lowest-precedence, right-associative
level; the ternary sits just above it, also right-associative.

The parser implements the binary levels by precedence climbing, so an operator chain
of any length parses without deep recursion. Nesting (parentheses, blocks, nested
statements and lambdas) is limited to 100000 levels, past which the parser reports
`nesting too deep`. A type nests at most 1000 levels (each pointer level, array
dimension, template argument list and fn type counts one, and so does the innermost
type), past which the parser reports `type nesting too deep`.

```
primary =
    INT_LIT | FLOAT_LIT | STRING_LIT | CHAR_LIT | 'true' | 'false' | 'null'
  | IDENT
  | array-lit                                          // { e0, e1, … }  (initializes an array)
  | IDENT struct-init                                  // Name { … }
  | IDENT '<' type (',' type)* '>' ( '(' arg-list? ')' | struct-init )  // turbofish call / templated literal
  | '(' expr ')'
  | lambda
  | 'sizeof' '(' type ')'           // a bare name, or a spelling over a known type (`*Node`)
  | 'sizeof' '(' expr ')'           // any other operand (`*p`, `a[0]`): its type, not evaluated
  | 'alloc_with' '(' expr ',' type ',' expr ')'
  | 'free_closure' '(' expr ')'
  | 'thread_create' '(' expr ')'

array-lit   = '{' ( expr (',' expr)* ','? )? '}'       // fewer elements than the size zero-fill
struct-init = '{' ( field-init (',' field-init)* )? '}'
field-init  = IDENT ':' expr        // named
            | expr                  // positional

lambda      = type '(' lambda-params? ')' block        // e.g. int (int x) { return x; }
lambda-params = ('escaping'? type IDENT) (',' 'escaping'? type IDENT)*
```

A struct literal is suppressed where a `{` would instead open a block, e.g. the
subject of `match`/`switch`/`if`/`while`/`for` conditions.

---

## See also

- [spec.md](spec.md): language semantics and the standard library.
- [../dev/abi.md](../dev/abi.md): how these constructs lower to LLVM IR.
