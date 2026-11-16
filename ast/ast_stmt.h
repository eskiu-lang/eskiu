// ast_stmt.h: the statement nodes (part of ast.h, which includes it in order: include ast.h, not this file).
#pragma once

// ============================================================================
// Statements
// ============================================================================

class Stmt : public ASTNode {
public:
    virtual ~Stmt() = default;
};

class BlockStmt : public Stmt {
public:
    ESKIU_NODE_KIND(BlockStmt)
    std::vector<BlockItem> items;  // Unified list of declarations and statements

    explicit BlockStmt(const std::vector<BlockItem>& blk_items = {})
        : items(blk_items) {}

    void accept(class ASTVisitor* visitor) override;
};

class IfStmt : public Stmt {
public:
    ESKIU_NODE_KIND(IfStmt)
    ExprPtr condition;
    StmtPtr thenBranch;
    StmtPtr elseBranch;

    IfStmt(ExprPtr cond, StmtPtr thenBr, StmtPtr elseBr = nullptr)
        : condition(std::move(cond)), thenBranch(std::move(thenBr)), elseBranch(std::move(elseBr)) {}

    void accept(class ASTVisitor* visitor) override;
};

class ForStmt : public Stmt {
public:
    ESKIU_NODE_KIND(ForStmt)
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
    ESKIU_NODE_KIND(ForInStmt)
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
    ESKIU_NODE_KIND(WhileStmt)
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
    ESKIU_NODE_KIND(DoWhileStmt)
    StmtPtr body;
    ExprPtr condition;
    std::string label;   // labeled-loop name ("" = unlabeled)

    DoWhileStmt(StmtPtr body, ExprPtr cond)
        : body(std::move(body)), condition(std::move(cond)) {}

    void accept(class ASTVisitor* visitor) override;
};

class ReturnStmt : public Stmt {
public:
    ESKIU_NODE_KIND(ReturnStmt)
    ExprPtr value;

    explicit ReturnStmt(ExprPtr val = nullptr) : value(std::move(val)) {}

    void accept(class ASTVisitor* visitor) override;
};

class BreakStmt : public Stmt {
public:
    ESKIU_NODE_KIND(BreakStmt)
    std::string label;   // target loop label ("" = innermost loop)
    void accept(class ASTVisitor* visitor) override;
};

class ContinueStmt : public Stmt {
public:
    ESKIU_NODE_KIND(ContinueStmt)
    std::string label;   // target loop label ("" = innermost loop)
    void accept(class ASTVisitor* visitor) override;
};

class SwitchStmt : public Stmt {
public:
    ESKIU_NODE_KIND(SwitchStmt)
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
    ESKIU_NODE_KIND(MatchStmt)
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
    ESKIU_NODE_KIND(ThrowStmt)
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
    ESKIU_NODE_KIND(TryStmt)
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
    ESKIU_NODE_KIND(DeferStmt)
    StmtPtr body;
    bool    isErr = false;

    DeferStmt(StmtPtr body, bool isErr) : body(std::move(body)), isErr(isErr) {}

    void accept(class ASTVisitor* visitor) override;
};

// thread_join(*void tid)
class ThreadJoinStmt : public Stmt {
public:
    ESKIU_NODE_KIND(ThreadJoinStmt)
    ExprPtr tid;
    explicit ThreadJoinStmt(ExprPtr t) : tid(std::move(t)) {}
    void accept(class ASTVisitor* visitor) override;
};

// Inline assembly: asm("cli") or asm("op" : "=r"(y) : "r"(x) : "rax")
class AsmStmt : public Stmt {
public:
    ESKIU_NODE_KIND(AsmStmt)
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
    ESKIU_NODE_KIND(ExprStmt)
    ExprPtr expr;

    explicit ExprStmt(ExprPtr e) : expr(std::move(e)) {}

    void accept(class ASTVisitor* visitor) override;
};
