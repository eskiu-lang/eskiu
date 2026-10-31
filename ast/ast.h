#pragma once

#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <utility>
#include <map>

// Operator overloading: an `operator <op>(...)` decl compiles to a normal function under a
// canonical mangled name, and a binary/unary expression whose operands are not built-in
// numeric/pointer types resolves to that function by the same name. The op is spelled as a
// word so the name stays a valid identifier (`+`->`add`, `[]`->`index`, unary `-`->`neg`).
inline std::string eskiuOpWord(const std::string& op) {
    static const char* const words[][2] = {
        {"+", "add"},   {"-", "sub"},   {"*", "mul"},   {"/", "div"},   {"%", "mod"},
        {"==", "eq"},   {"!=", "ne"},   {"<", "lt"},    {">", "gt"},    {"<=", "le"},
        {">=", "ge"},   {"&", "band"},  {"|", "bor"},   {"^", "bxor"},  {"<<", "shl"},
        {">>", "shr"},  {"[]", "index"}, {"u-", "neg"}, {"!", "lnot"},  {"~", "bnot"}};
    for (const auto& w : words)
        if (op == w[0]) return w[1];
    return "";
}
// Make a type spelling safe to embed in a symbol name (`*V3`->`p_V3`, `List<int>`->`List_int`,
// `V[]`->`V_A_`, so an array or slice never mangles like its element type).
// A `struct:`/`union:`/`enum:`/`interface:` prefix (how sema spells a nominal expr type) is
// stripped first, so a decl's written param type `V3` and a call operand's `struct:V3` mangle
// identically.
inline std::string eskiuTyMangle(const std::string& tin) {
    std::string t = tin;
    for (const char* pfx : {"struct:", "union:", "enum:", "interface:"}) {
        std::string p = pfx;
        if (t.rfind(p, 0) == 0) { t = t.substr(p.size()); break; }
    }
    std::string r;
    for (char c : t) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') r += c;
        else if (c == '*') r += "p_";
        else if (c == '[') r += "_A";
        else if (c == ']') r += "_";
        // '<', '>', ' ', ',' etc. are dropped
    }
    return r;
}
// Canonical function name for an operator over the given operand type spellings.
inline std::string eskiuOpName(const std::string& op, const std::vector<std::string>& tys) {
    std::string w = eskiuOpWord(op);
    if (w.empty()) return "";
    std::string n = "__op_" + w;
    for (const auto& t : tys) n += "_" + eskiuTyMangle(t);
    return n;
}

// Forward declarations
class ASTNode;
class Expr;
class Stmt;
class Decl;

// Smart pointers
using ASTNodePtr = std::shared_ptr<ASTNode>;
using ExprPtr = std::shared_ptr<Expr>;
using StmtPtr = std::shared_ptr<Stmt>;
using DeclPtr = std::shared_ptr<Decl>;

// BlockItem: can be either a declaration or a statement (allows interleaving)
using BlockItem = std::variant<DeclPtr, StmtPtr>;

// ============================================================================
// Base Classes
// ============================================================================

class ASTNode {
public:
    int line = 0;
    int col  = 0;
    virtual ~ASTNode() = default;
    virtual void accept(class ASTVisitor* visitor) = 0;
};

// ============================================================================
// Declarations
// ============================================================================

class Decl : public ASTNode {
public:
    std::string name;
    // The source file a top-level declaration was parsed from (stamped by the
    // parser; "" when synthesized). Diagnostics inside it name this file.
    std::string sourceFile;
    // Parsed from an `import`ed file (a library): -Wall does not report its unused
    // functions, as C does not for another translation unit's.
    bool fromImport = false;
    explicit Decl(const std::string& name) : name(name) {}
    virtual ~Decl() = default;
};

class FunctionDecl : public Decl {
public:
    std::string returnType;
    std::vector<std::pair<std::string, std::string>> params; // (type, name)
    StmtPtr body;
    std::vector<std::string> typeParams; // non-empty → template function
    // Bounded generics: type-param name → interface constraint(s), e.g.
    // `<T: Ord>` or `<K: Hashable + Eq>`. A concrete type arg must satisfy
    // every listed interface at instantiation.
    std::map<std::string, std::vector<std::string>> constraints;
    // Per-param `escaping` flag (parallel to params): the param retains the
    // closure beyond the call, so closures passed there get a heap env.
    std::vector<bool> paramEscaping;
    // Per-param (line, col) of the parameter name (parallel to params; may be
    // empty for synthesized functions): diagnostics about a parameter point here.
    std::vector<std::pair<int, int>> paramPositions;
    // Per-param default value (`int port = 443`; parallel to params, null = none) and its
    // source text (for the AST printer). A call that omits trailing arguments gets them.
    std::vector<ExprPtr> paramDefaults;
    std::vector<std::string> paramDefaultTexts;
    // `async fn`: the call yields `*Future<returnType>`; the body is lowered to a
    // resumable state machine by the async transform. Declared return type stays
    // in `returnType` (the inner T).
    bool isAsync = false;
    // `must_use`: discarding a call to this function (a bare call statement) is an error.
    bool mustUse = false;
    // `deprecated("use X") T f(...)`: each use outside an imported module warns, with
    // this message (empty: not deprecated).
    std::string deprecatedMsg;
    // Operator overload: the op this `operator <op>(...)` implements ("+","[]","u-",...);
    // empty for a normal function. Sema registers it so `a op b` resolves here by operands.
    std::string operatorSym;

    FunctionDecl(const std::string& name, const std::string& returnType,
                 const std::vector<std::pair<std::string, std::string>>& params,
                 StmtPtr body)
        : Decl(name), returnType(returnType), params(params), body(std::move(body)) {}

    void accept(class ASTVisitor* visitor) override;
};

class VarDecl : public Decl {
public:
    std::string type;
    ExprPtr initializer;
    bool isVolatile = false;
    bool isConst = false;
    bool isStatic = false;   // `static` local: one instance, persists across calls
    bool isExtern = false;   // `extern <type> <name>;` — a global defined in another
                             // translation unit (C interop); external linkage, no init
    bool scriptInit = false; // a script's global: zero at start, its initializer runs in
                             // order in the implicit main (any expression, not a constant)
    bool rangeBound = false; // a `for (i in A..B)` desugar decl: sema retypes it to the
                             // bounds' common integer type (ty::rangeVarType)

    VarDecl(const std::string& name, const std::string& type, ExprPtr init = nullptr)
        : Decl(name), type(type), initializer(std::move(init)) {}

    void accept(class ASTVisitor* visitor) override;
};

class InterfaceDecl : public Decl {
public:
    // Each method signature: (return_type, name, param_types)
    struct MethodSig {
        std::string returnType;
        std::string name;
        std::vector<std::pair<std::string, std::string>> params;
    };
    std::vector<MethodSig> methods;

    explicit InterfaceDecl(const std::string& name) : Decl(name) {}

    void accept(class ASTVisitor* visitor) override;
};

class StructDecl : public Decl {
public:
    struct Field {
        std::string type;
        std::string name;
        int bitWidth = 0;   // >0 for a bitfield (e.g. `uint32 x : 1;`), 0 otherwise, -1 for `: 0`
        bool unnamed = false;   // only in layoutFields(): an unnamed bitfield (width 0 = `: 0`)
        int line = 0, col = 0;  // the field name's position (editor tooling)
        ExprPtr defaultValue = nullptr;   // `int t = 30;`: the value an omitted or unset field takes
    };
    // An unnamed bitfield `T : N;`: padding laid out like a bitfield of type T, where
    // N = 0 moves the next bitfield to a new unit of T. It is not a member, so it is kept
    // apart from `fields`; `before` is the index of the named field it precedes.
    struct Pad {
        std::string type;
        int bitWidth = 0;
        size_t before = 0;
    };
    std::vector<Field> fields;
    std::vector<Pad> pads;
    std::vector<DeclPtr> methods;
    std::vector<std::string> typeParams; // non-empty → this is a template
    // Bounded generics: type-param name → interface constraint(s) (`<K: Hashable>`).
    std::map<std::string, std::vector<std::string>> constraints;
    bool isPacked = false;               // `packed struct` or under `#pragma pack(1)`
    int  packAlign = 0;                  // `#pragma pack(N)`: cap field alignment at N
                                         // (0 = natural; 1 = fully packed == isPacked)

    StructDecl(const std::string& name, const std::vector<Field>& fields)
        : Decl(name), fields(fields) {}

    void accept(class ASTVisitor* visitor) override;
};

// union MyUnion { int i; float f; *uint8 b; }
// All fields share offset 0. Size = sizeof(largest field).
// Field access uses a cast to the field type.
class UnionDecl : public Decl {
public:
    std::vector<StructDecl::Field> fields;
    std::vector<StructDecl::Pad> pads;   // unnamed bitfields (size and alignment only)
    int packAlign = 0;                   // `#pragma pack(N)`: cap member alignment at N (0 = natural)

    UnionDecl(const std::string& name, const std::vector<StructDecl::Field>& fields)
        : Decl(name), fields(fields) {}

    void accept(class ASTVisitor* visitor) override;
};

// The fields of a struct or union in declaration order with its unnamed bitfields among
// them (marked `unnamed`), for the layout code.
inline std::vector<StructDecl::Field> layoutFields(const std::vector<StructDecl::Field>& fields,
                                                   const std::vector<StructDecl::Pad>& pads) {
    std::vector<StructDecl::Field> out;
    size_t p = 0;
    for (size_t i = 0; i <= fields.size(); ++i) {
        for (; p < pads.size() && pads[p].before <= i; ++p)
            out.push_back({pads[p].type, "", pads[p].bitWidth, true});
        if (i < fields.size()) out.push_back(fields[i]);
    }
    return out;
}

class ExternDecl : public Decl {
public:
    std::string returnType;
    std::vector<std::pair<std::string, std::string>> params;
    std::vector<bool> paramEscaping;   // per-param `escaping` flag

    ExternDecl(const std::string& name, const std::string& returnType,
               const std::vector<std::pair<std::string, std::string>>& params)
        : Decl(name), returnType(returnType), params(params) {}

    void accept(class ASTVisitor* visitor) override;
};

// intrinsic int atomic_cas(*int cell, int expected, int desired);
// Syntactically an extern prototype, but a call lowers to inline IR (e.g. an
// LLVM atomic), NOT to a call to an external C symbol. Codegen recognises the
// name via its intrinsic registry and emits the operation directly; no `declare`
// is produced. Distinct from ExternDecl so the two intents never blur.
class IntrinsicDecl : public Decl {
public:
    std::string returnType;
    std::vector<std::pair<std::string, std::string>> params;
    std::vector<bool> paramEscaping;   // per-param `escaping` flag

    IntrinsicDecl(const std::string& name, const std::string& returnType,
                  const std::vector<std::pair<std::string, std::string>>& params)
        : Decl(name), returnType(returnType), params(params) {}

    void accept(class ASTVisitor* visitor) override;
};

// enum Color { Red, Green, Blue }  or  enum Status { Ok = 0, Err = 2 }
// Members are int constants. An enum type maps to i32.
class EnumDecl : public Decl {
public:
    std::vector<std::pair<std::string, long long>> members; // (name, value)
    // Per-variant payload field types, parallel to `members` (empty = no payload).
    // If any variant has a payload, this enum is an algebraic data type: a tagged
    // union laid out as { i32 tag; <storage for the largest variant> }, rather than
    // a plain integer enum. Variants are constructed by name (`Circle(2.0)`, or a
    // bare `None`) and consumed with `match`.
    std::vector<std::vector<std::string>> payloads;
    // Per-member value expression, parallel to `members` (null = a literal value or the
    // previous member's value + 1). `B = A << 2`: an integer constant expression the type
    // checker folds into `members` (literals, earlier members, `const` ints, `sizeof`).
    std::vector<ExprPtr> valueExprs;
    // Non-empty → a generic algebraic enum (e.g. `enum Option<T> { None, Some(T) }`),
    // monomorphized per instantiation like a template struct.
    std::vector<std::string> typeParams;

    EnumDecl(const std::string& name,
             std::vector<std::pair<std::string, long long>> members)
        : Decl(name), members(std::move(members)) {}

    bool isADT() const {
        for (const auto& p : payloads) if (!p.empty()) return true;
        return false;
    }

    void accept(class ASTVisitor* visitor) override;
};

// type Alias = UnderlyingType;  — a name for an existing type.
class TypeAliasDecl : public Decl {
public:
    std::string aliased; // the underlying type string

    TypeAliasDecl(const std::string& name, std::string aliased)
        : Decl(name), aliased(std::move(aliased)) {}

    void accept(class ASTVisitor* visitor) override;
};

// ============================================================================
// Statements
// ============================================================================

class Stmt : public ASTNode {
public:
    virtual ~Stmt() = default;
};

class BlockStmt : public Stmt {
public:
    std::vector<BlockItem> items;  // Unified list of declarations and statements

    explicit BlockStmt(const std::vector<BlockItem>& blk_items = {})
        : items(blk_items) {}

    void accept(class ASTVisitor* visitor) override;
};

class IfStmt : public Stmt {
public:
    ExprPtr condition;
    StmtPtr thenBranch;
    StmtPtr elseBranch;

    IfStmt(ExprPtr cond, StmtPtr thenBr, StmtPtr elseBr = nullptr)
        : condition(std::move(cond)), thenBranch(std::move(thenBr)), elseBranch(std::move(elseBr)) {}

    void accept(class ASTVisitor* visitor) override;
};

class ForStmt : public Stmt {
public:
    StmtPtr init;
    ExprPtr condition;
    ExprPtr step;
    StmtPtr body;
    std::string label;   // labeled-loop name ("" = unlabeled); target of `break label` / `continue label`

    ForStmt(StmtPtr init, ExprPtr cond, ExprPtr step, StmtPtr body)
        : init(std::move(init)), condition(std::move(cond)), step(std::move(step)), body(std::move(body)) {}

    void accept(class ASTVisitor* visitor) override;
};

// for (x in iterable) — iterates over a fixed-size array or a List<T>.
class ForInStmt : public Stmt {
public:
    std::string varName;
    ExprPtr iterable;
    StmtPtr body;
    // Stamped by the type checker (consumed by the async transform's for-in
    // desugar, which has no type info of its own): the loop variable's element
    // type, whether the iterable is a fixed-size array (vs a List-like struct
    // with `data`/`size`), and the array dimension `N` (literal / const / enum
    // name) when it is an array.
    std::string resolvedElemType;
    std::string resolvedIterType;   // the iterable's type (the async lowering holds a non-place iterable in a local)
    bool        isArrayIter = false;
    std::string arrayDim;
    std::string label;   // labeled-loop name ("" = unlabeled)

    ForInStmt(std::string varName, ExprPtr iterable, StmtPtr body)
        : varName(std::move(varName)), iterable(std::move(iterable)), body(std::move(body)) {}

    void accept(class ASTVisitor* visitor) override;
};

class WhileStmt : public Stmt {
public:
    ExprPtr condition;
    StmtPtr body;
    std::string label;   // labeled-loop name ("" = unlabeled)

    WhileStmt(ExprPtr cond, StmtPtr body)
        : condition(std::move(cond)), body(std::move(body)) {}

    void accept(class ASTVisitor* visitor) override;
};

// do { body } while (cond); — the body runs at least once, then repeats while cond.
class DoWhileStmt : public Stmt {
public:
    StmtPtr body;
    ExprPtr condition;
    std::string label;   // labeled-loop name ("" = unlabeled)

    DoWhileStmt(StmtPtr body, ExprPtr cond)
        : body(std::move(body)), condition(std::move(cond)) {}

    void accept(class ASTVisitor* visitor) override;
};

class ReturnStmt : public Stmt {
public:
    ExprPtr value;

    explicit ReturnStmt(ExprPtr val = nullptr) : value(std::move(val)) {}

    void accept(class ASTVisitor* visitor) override;
};

class BreakStmt : public Stmt {
public:
    std::string label;   // target loop label ("" = innermost loop)
    void accept(class ASTVisitor* visitor) override;
};

class ContinueStmt : public Stmt {
public:
    std::string label;   // target loop label ("" = innermost loop)
    void accept(class ASTVisitor* visitor) override;
};

class SwitchStmt : public Stmt {
public:
    struct Case {
        ExprPtr value;               // nullptr = default
        // A declaration may appear directly after the label (C): its scope is the rest
        // of the switch body, so the whole switch is one scope.
        std::vector<BlockItem> stmts;
    };
    ExprPtr subject;
    std::vector<Case> cases;

    SwitchStmt(ExprPtr subj, std::vector<Case> cases)
        : subject(std::move(subj)), cases(std::move(cases)) {}

    void accept(class ASTVisitor* visitor) override;
};

// match subject { Variant(b0, b1) -> stmt;  Other -> stmt;  _ -> stmt; }
// Destructures an algebraic-enum value: dispatches on its tag and binds the
// active variant's payload fields to names in that arm.
class MatchStmt : public Stmt {
public:
    struct Arm {
        std::string variant;                 // variant name, or "" for the `_` default
        std::vector<std::string> bindings;   // payload binding names (for this variant)
        std::vector<std::string> bindingTypes;   // stamped by the type checker (the async lowering's frame fields)
        // In a generic async function: per checked instance, its type-argument bindings
        // and the binding types (the async lowering generalizes them, as for awaits).
        std::vector<std::pair<std::map<std::string, std::string>, std::vector<std::string>>> instanceBindingTypes;
        StmtPtr body;
    };
    ExprPtr subject;
    std::vector<Arm> arms;
    std::string enumName;                     // filled by the type checker (subject's enum)

    MatchStmt(ExprPtr subject, std::vector<Arm> arms)
        : subject(std::move(subject)), arms(std::move(arms)) {}

    void accept(class ASTVisitor* visitor) override;
};

// throw expr;
class ThrowStmt : public Stmt {
public:
    ExprPtr     value;
    std::string valueType; // filled in by TypeChecker
    // Synthesized by the async lowering only: `value` is an exception object a capturing
    // catch clause copied (CatchClause::captureAll), thrown again as is.
    bool        rethrowCaptured = false;
    explicit ThrowStmt(ExprPtr v) : value(std::move(v)) {}
    void accept(class ASTVisitor* visitor) override;
};

// try { body } catch (Type name) { handler } finally { cleanup }
class TryStmt : public Stmt {
public:
    struct CatchClause {
        std::string type;
        std::string name;
        StmtPtr     body;
        int         line = 0;   // of the variable name
        int         col = 0;
        // Synthesized by the async lowering only: matches any exception and binds `name`
        // (a `*uint8`) to a copy of the exception object, for a later rethrow
        // (ThrowStmt::rethrowCaptured).
        bool        captureAll = false;
    };
    StmtPtr               body;
    std::vector<CatchClause> catches;
    StmtPtr               finally; // may be nullptr
    // Synthesized by the async lowering only: `finally` runs only when an exception leaves
    // the try (no catch matched), not on a normal or early exit.
    bool                  unwindOnly = false;

    TryStmt(StmtPtr body, std::vector<CatchClause> catches, StmtPtr finally)
        : body(std::move(body)), catches(std::move(catches)),
          finally(std::move(finally)) {}

    void accept(class ASTVisitor* visitor) override;
};

// `defer stmt;` runs stmt at exit of the enclosing block, in LIFO order, on every
// path out (fall-through, return, break, continue, `?`-propagation, exception).
// `isErr` (errdefer, Slice 2) restricts it to the error-exit paths.
class DeferStmt : public Stmt {
public:
    StmtPtr body;
    bool    isErr = false;

    DeferStmt(StmtPtr body, bool isErr) : body(std::move(body)), isErr(isErr) {}

    void accept(class ASTVisitor* visitor) override;
};

// thread_join(*void tid)
class ThreadJoinStmt : public Stmt {
public:
    ExprPtr tid;
    explicit ThreadJoinStmt(ExprPtr t) : tid(std::move(t)) {}
    void accept(class ASTVisitor* visitor) override;
};

// Inline assembly: asm("cli") or asm("op" : "=r"(y) : "r"(x) : "rax")
class AsmStmt : public Stmt {
public:
    std::string asmString;
    // Extended asm: each entry is (constraint, expression). An output's constraint
    // starts with `=` (written) or `+` (read and written); its expression is an lvalue.
    std::vector<std::pair<std::string, ExprPtr>> outputs;
    std::vector<std::pair<std::string, ExprPtr>> inputs;
    std::vector<std::string> clobbers;

    AsmStmt(std::string asmStr,
            std::vector<std::pair<std::string, ExprPtr>> outputs = {},
            std::vector<std::pair<std::string, ExprPtr>> inputs = {},
            std::vector<std::string> clobbers = {})
        : asmString(std::move(asmStr)),
          outputs(std::move(outputs)),
          inputs(std::move(inputs)),
          clobbers(std::move(clobbers)) {}

    void accept(class ASTVisitor* visitor) override;
};

class ExprStmt : public Stmt {
public:
    ExprPtr expr;

    explicit ExprStmt(ExprPtr e) : expr(std::move(e)) {}

    void accept(class ASTVisitor* visitor) override;
};

// ============================================================================
// Expressions
// ============================================================================

class Expr : public ASTNode {
public:
    virtual ~Expr() = default;
    // Codegen's memo of whether this place is reached from a `volatile` variable, valid
    // while its set of volatile names has `volMemoSize` entries (CodeGen::volatileRooted).
    mutable size_t volMemoSize = (size_t)-1;
    mutable bool volMemo = false;
};

class BinaryExpr : public Expr {
public:
    ExprPtr left;
    std::string op;
    ExprPtr right;
    std::string opFunc;   // set by sema when this resolves to a user operator overload ("" = built-in)
    // A script's `x = <initializer>` in the implicit main: the initializer is the
    // global's own, already checked at its declaration.
    bool scriptInit = false;

    BinaryExpr(ExprPtr left, const std::string& op, ExprPtr right)
        : left(std::move(left)), op(op), right(std::move(right)) {}
    // Frees a long left-leaning chain with a loop, not one nested destructor per operator.
    ~BinaryExpr() override;

    void accept(class ASTVisitor* visitor) override;
};

class UnaryExpr : public Expr {
public:
    std::string op;
    ExprPtr operand;
    std::string opFunc;   // set by sema when this resolves to a user unary operator overload

    UnaryExpr(const std::string& op, ExprPtr operand)
        : op(op), operand(std::move(operand)) {}

    void accept(class ASTVisitor* visitor) override;
};

// `x++` / `x--` (postfix) and `++x` / `--x` (prefix). The operand is an lvalue;
// the expression value is the old value (postfix) or the new value (prefix).
class IncDecExpr : public Expr {
public:
    ExprPtr operand;
    bool    decrement;   // false = ++, true = --
    bool    prefix;      // true = ++x/--x, false = x++/x--

    IncDecExpr(ExprPtr operand, bool decrement, bool prefix)
        : operand(std::move(operand)), decrement(decrement), prefix(prefix) {}

    void accept(class ASTVisitor* visitor) override;
};

// Postfix `?` error-propagation operator: `expr?` on a Result<T,E>.
// If the result is Err, returns it from the enclosing function; otherwise the
// expression evaluates to the unwrapped success value (the Result's T).
class QuestionExpr : public Expr {
public:
    ExprPtr operand;

    explicit QuestionExpr(ExprPtr operand) : operand(std::move(operand)) {}

    void accept(class ASTVisitor* visitor) override;
};

// Ternary conditional `cond ? thenExpr : elseExpr`. Exactly one arm is evaluated;
// the arms must share a common type, which is the expression's type.
class TernaryExpr : public Expr {
public:
    ExprPtr condition;
    ExprPtr thenExpr;
    ExprPtr elseExpr;

    TernaryExpr(ExprPtr condition, ExprPtr thenExpr, ExprPtr elseExpr)
        : condition(std::move(condition)), thenExpr(std::move(thenExpr)),
          elseExpr(std::move(elseExpr)) {}

    void accept(class ASTVisitor* visitor) override;
};

class CallExpr : public Expr {
public:
    ExprPtr callee;
    std::vector<ExprPtr> args;
    // `dbg(e)`: the source text of `e` (the builtin prints it with the value), and
    // whether the type checker resolved the call to the builtin (no user `dbg`).
    std::string dbgText;
    bool dbgBuiltin = false;

    CallExpr(ExprPtr callee, const std::vector<ExprPtr>& args = {})
        : callee(std::move(callee)), args(args) {}

    void accept(class ASTVisitor* visitor) override;
};

class IndexExpr : public Expr {
public:
    ExprPtr base;
    ExprPtr index;
    ExprPtr highIndex;   // set for a slice expression `base[index..highIndex]`; else null
    std::string opFunc;  // set by sema when `base[index]` resolves to a user `operator [](B, I)`

    IndexExpr(ExprPtr base, ExprPtr index, ExprPtr highIndex = nullptr)
        : base(std::move(base)), index(std::move(index)), highIndex(std::move(highIndex)) {}

    bool isSlice() const { return highIndex != nullptr; }

    void accept(class ASTVisitor* visitor) override;
};

class MemberExpr : public Expr {
public:
    ExprPtr base;
    std::string member;
    int memberLine = 0, memberCol = 0;   // the member name's position (editor tooling)

    MemberExpr(ExprPtr base, const std::string& member)
        : base(std::move(base)), member(member) {}

    void accept(class ASTVisitor* visitor) override;
};

class CastExpr : public Expr {
public:
    std::string targetType;
    ExprPtr expr;

    CastExpr(const std::string& type, ExprPtr expr)
        : targetType(type), expr(std::move(expr)) {}

    void accept(class ASTVisitor* visitor) override;
};

class LiteralExpr : public Expr {
public:
    enum class Kind { INT, FLOAT, STRING, CHAR, BOOL, NULL_VAL };

    Kind kind;
    std::string value;

    LiteralExpr(Kind kind, const std::string& value)
        : kind(kind), value(value) {}

    void accept(class ASTVisitor* visitor) override;
};

class IdentExpr : public Expr {
public:
    std::string name;

    explicit IdentExpr(const std::string& name) : name(name) {}

    void accept(class ASTVisitor* visitor) override;
};

// alloc_with(&allocator, T, N) — allocate N*sizeof(T) bytes from an explicit
// allocator (a struct providing `*void alloc(...)`), returning *T.
class AllocWithExpr : public Expr {
public:
    ExprPtr allocator;     // the allocator (typically &someAllocator)
    std::string elemType;  // T
    ExprPtr count;         // N

    AllocWithExpr(ExprPtr allocator, const std::string& type, ExprPtr count)
        : allocator(std::move(allocator)), elemType(type), count(std::move(count)) {}

    void accept(class ASTVisitor* visitor) override;
};

class TemplateCallExpr : public Expr {
public:
    std::string templateName;
    std::vector<std::string> typeArgs;
    std::vector<ExprPtr> args;

    TemplateCallExpr(std::string name, std::vector<std::string> typeArgs, std::vector<ExprPtr> args)
        : templateName(std::move(name)), typeArgs(std::move(typeArgs)), args(std::move(args)) {}

    void accept(class ASTVisitor* visitor) override;
};

class StructInitExpr : public Expr {
public:
    std::string structName;
    // pair: (field_name, value) — field_name is empty string for positional init
    std::vector<std::pair<std::string, ExprPtr>> fieldInits;
    // (line, col) of each named field (parallel to fieldInits; 0 for a positional one).
    std::vector<std::pair<int, int>> fieldPositions;

    StructInitExpr(const std::string& name,
                   std::vector<std::pair<std::string, ExprPtr>> inits)
        : structName(name), fieldInits(std::move(inits)) {}

    void accept(class ASTVisitor* visitor) override;
};

// Array literal `{ e0, e1, ... }` — untyped; the target array type gives the
// element type and size. Fewer elements than the size zero-fill the rest.
class ArrayLitExpr : public Expr {
public:
    std::vector<ExprPtr> elements;
    explicit ArrayLitExpr(std::vector<ExprPtr> elements)
        : elements(std::move(elements)) {}
    void accept(class ASTVisitor* visitor) override;
};

// Lambda / anonymous function: fn(int x) -> int { return x * 2; }
class LambdaExpr : public Expr {
public:
    std::vector<std::pair<std::string, std::string>> params; // (type, name)
    std::string returnType;
    StmtPtr body;
    // Populated by TypeChecker: variables captured from enclosing scope
    std::vector<std::pair<std::string, std::string>> captures; // (name, type)
    std::vector<bool> paramEscaping;   // per-param `escaping` flag
    // Set by analysis: true if this closure may outlive its creating function
    // (escapes) and therefore needs a heap-allocated environment.
    bool escapes = true;   // sound default: heap unless proven non-escaping
    // A call statement's trailing block (`f(a) { ... }`), parsed into this lambda.
    bool trailing = false;

    LambdaExpr(std::vector<std::pair<std::string, std::string>> params,
               std::string returnType, StmtPtr body)
        : params(std::move(params)), returnType(std::move(returnType)),
          body(std::move(body)) {}

    void accept(class ASTVisitor* visitor) override;
};

// sizeof(T) -> int64
class SizeofExpr : public Expr {
public:
    std::string typeName;
    // `sizeof(expr)` (typeName empty): the size of the operand's type. The operand is
    // type-checked but never evaluated.
    ExprPtr operand;
    explicit SizeofExpr(std::string t) : typeName(std::move(t)) {}
    void accept(class ASTVisitor* visitor) override;
};

// thread_create(fn()->void) -> *void  (returns thread handle)
class ThreadCreateExpr : public Expr {
public:
    ExprPtr worker;
    explicit ThreadCreateExpr(ExprPtr w) : worker(std::move(w)) {}
    void accept(class ASTVisitor* visitor) override;
};

// await E — suspend the enclosing async fn until the Future E completes, then
// yield its value. E must have type `*Future<T>`; the result type is T. Legal
// only inside an `async fn`. Lowered by the async transform.
class AwaitExpr : public Expr {
public:
    ExprPtr operand;
    std::string resolvedType;   // set by the type checker: the awaited value type T'
    // In a generic async function: per checked instance, its type-argument bindings and
    // the awaited type (source form). The async transform recovers the type in terms of
    // the template's parameters from these (the body is shared by every instance).
    std::vector<std::pair<std::map<std::string, std::string>, std::string>> instanceTypes;
    explicit AwaitExpr(ExprPtr o) : operand(std::move(o)) {}
    void accept(class ASTVisitor* visitor) override;
};

// free_closure(f) -> void — release the heap environment of an escaping closure.
// The argument is any closure value (fn(...)->R fat pointer); this frees its env
// (slot 1 of the fat pointer). A no-op for non-capturing closures (null env).
class FreeClosureExpr : public Expr {
public:
    ExprPtr closure;
    explicit FreeClosureExpr(ExprPtr c) : closure(std::move(c)) {}
    void accept(class ASTVisitor* visitor) override;
};

// ============================================================================
// Program (root node)
// ============================================================================

class Program : public ASTNode {
public:
    std::vector<DeclPtr> declarations;
    std::vector<std::string> linkLibs;   // `#pragma link("name")`, deduped, in order

    explicit Program(const std::vector<DeclPtr>& decls = {})
        : declarations(decls) {}

    void accept(class ASTVisitor* visitor) override;
};

// ============================================================================
// Visitor pattern for AST traversal
// ============================================================================

class ASTVisitor {
public:
    virtual ~ASTVisitor() = default;

    virtual void visit(Program* node) = 0;
    virtual void visit(FunctionDecl* node) = 0;
    virtual void visit(VarDecl* node) = 0;
    virtual void visit(StructDecl* node) = 0;
    virtual void visit(ExternDecl* node) = 0;
    virtual void visit(IntrinsicDecl* node) = 0;
    virtual void visit(InterfaceDecl* node) = 0;
    virtual void visit(EnumDecl* node) = 0;
    virtual void visit(TypeAliasDecl* node) = 0;
    virtual void visit(BlockStmt* node) = 0;
    virtual void visit(IfStmt* node) = 0;
    virtual void visit(ForStmt* node) = 0;
    virtual void visit(ForInStmt* node) = 0;
    virtual void visit(WhileStmt* node) = 0;
    virtual void visit(DoWhileStmt* node) = 0;
    virtual void visit(ReturnStmt* node) = 0;
    virtual void visit(BreakStmt* node) = 0;
    virtual void visit(ContinueStmt* node) = 0;
    virtual void visit(SwitchStmt* node) = 0;
    virtual void visit(MatchStmt* node) = 0;
    virtual void visit(ExprStmt* node) = 0;
    virtual void visit(BinaryExpr* node) = 0;
    virtual void visit(UnaryExpr* node) = 0;
    virtual void visit(IncDecExpr* node) = 0;
    virtual void visit(QuestionExpr* node) = 0;
    virtual void visit(TernaryExpr* node) = 0;
    virtual void visit(CallExpr* node) = 0;
    virtual void visit(IndexExpr* node) = 0;
    virtual void visit(MemberExpr* node) = 0;
    virtual void visit(CastExpr* node) = 0;
    virtual void visit(LiteralExpr* node) = 0;
    virtual void visit(IdentExpr* node) = 0;
    virtual void visit(StructInitExpr* node) = 0;
    virtual void visit(ArrayLitExpr* node) = 0;
    virtual void visit(AllocWithExpr* node) = 0;
    virtual void visit(TemplateCallExpr* node) = 0;
    virtual void visit(LambdaExpr* node) = 0;
    virtual void visit(AsmStmt* node) = 0;
    virtual void visit(UnionDecl* node) = 0;
    virtual void visit(SizeofExpr* node) = 0;
    virtual void visit(ThreadCreateExpr* node) = 0;
    virtual void visit(FreeClosureExpr* node) = 0;
    virtual void visit(AwaitExpr* node) = 0;
    virtual void visit(ThreadJoinStmt* node) = 0;
    virtual void visit(ThrowStmt* node) = 0;
    virtual void visit(TryStmt* node) = 0;
    virtual void visit(DeferStmt* node) = 0;
};
