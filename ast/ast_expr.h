// ast_expr.h: the expression nodes (part of ast.h, which includes it in order: include ast.h, not this file).
#pragma once

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
    ESKIU_NODE_KIND(BinaryExpr)
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
    ESKIU_NODE_KIND(UnaryExpr)
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
    ESKIU_NODE_KIND(IncDecExpr)
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
    ESKIU_NODE_KIND(QuestionExpr)
    ExprPtr operand;

    explicit QuestionExpr(ExprPtr operand) : operand(std::move(operand)) {}

    void accept(class ASTVisitor* visitor) override;
};

// Ternary conditional `cond ? thenExpr : elseExpr`. Exactly one arm is evaluated;
// the arms must share a common type, which is the expression's type.
class TernaryExpr : public Expr {
public:
    ESKIU_NODE_KIND(TernaryExpr)
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
    ESKIU_NODE_KIND(CallExpr)
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
    ESKIU_NODE_KIND(IndexExpr)
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
    ESKIU_NODE_KIND(MemberExpr)
    ExprPtr base;
    std::string member;
    int memberLine = 0, memberCol = 0;   // the member name's position (editor tooling)

    MemberExpr(ExprPtr base, const std::string& member)
        : base(std::move(base)), member(member) {}

    void accept(class ASTVisitor* visitor) override;
};

class CastExpr : public Expr {
public:
    ESKIU_NODE_KIND(CastExpr)
    std::string targetType;
    ExprPtr expr;

    CastExpr(const std::string& type, ExprPtr expr)
        : targetType(type), expr(std::move(expr)) {}

    void accept(class ASTVisitor* visitor) override;
};

class LiteralExpr : public Expr {
public:
    ESKIU_NODE_KIND(LiteralExpr)
    enum class Kind { INT, FLOAT, STRING, CHAR, BOOL, NULL_VAL };

    Kind kind;
    std::string value;

    LiteralExpr(Kind kind, const std::string& value)
        : kind(kind), value(value) {}

    void accept(class ASTVisitor* visitor) override;
};

class IdentExpr : public Expr {
public:
    ESKIU_NODE_KIND(IdentExpr)
    std::string name;

    explicit IdentExpr(const std::string& name) : name(name) {}

    void accept(class ASTVisitor* visitor) override;
};

// alloc_with(&allocator, T, N) — allocate N*sizeof(T) bytes from an explicit
// allocator (a struct providing `*void alloc(...)`), returning *T.
class AllocWithExpr : public Expr {
public:
    ESKIU_NODE_KIND(AllocWithExpr)
    ExprPtr allocator;     // the allocator (typically &someAllocator)
    std::string elemType;  // T
    ExprPtr count;         // N

    AllocWithExpr(ExprPtr allocator, const std::string& type, ExprPtr count)
        : allocator(std::move(allocator)), elemType(type), count(std::move(count)) {}

    void accept(class ASTVisitor* visitor) override;
};

class TemplateCallExpr : public Expr {
public:
    ESKIU_NODE_KIND(TemplateCallExpr)
    std::string templateName;
    std::vector<std::string> typeArgs;
    std::vector<ExprPtr> args;

    TemplateCallExpr(std::string name, std::vector<std::string> typeArgs, std::vector<ExprPtr> args)
        : templateName(std::move(name)), typeArgs(std::move(typeArgs)), args(std::move(args)) {}

    void accept(class ASTVisitor* visitor) override;
};

class StructInitExpr : public Expr {
public:
    ESKIU_NODE_KIND(StructInitExpr)
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
    ESKIU_NODE_KIND(ArrayLitExpr)
    std::vector<ExprPtr> elements;
    explicit ArrayLitExpr(std::vector<ExprPtr> elements)
        : elements(std::move(elements)) {}
    void accept(class ASTVisitor* visitor) override;
};

// Lambda / anonymous function: fn(int x) -> int { return x * 2; }
class LambdaExpr : public Expr {
public:
    ESKIU_NODE_KIND(LambdaExpr)
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
    ESKIU_NODE_KIND(SizeofExpr)
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
    ESKIU_NODE_KIND(ThreadCreateExpr)
    ExprPtr worker;
    explicit ThreadCreateExpr(ExprPtr w) : worker(std::move(w)) {}
    void accept(class ASTVisitor* visitor) override;
};

// await E — suspend the enclosing async fn until the Future E completes, then
// yield its value. E must have type `*Future<T>`; the result type is T. Legal
// only inside an `async fn`. Lowered by the async transform.
class AwaitExpr : public Expr {
public:
    ESKIU_NODE_KIND(AwaitExpr)
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
    ESKIU_NODE_KIND(FreeClosureExpr)
    ExprPtr closure;
    explicit FreeClosureExpr(ExprPtr c) : closure(std::move(c)) {}
    void accept(class ASTVisitor* visitor) override;
};
