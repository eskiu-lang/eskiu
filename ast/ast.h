#pragma once

#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <utility>
#include <map>
#include <type_traits>

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

// The concrete class of a node. nodeCast<T>(p) is dynamic_cast<T*>(p) for a node class
// (every one is a leaf: nothing derives from FunctionDecl, BinaryExpr, ...) at the cost
// of one virtual call and a compare, where dynamic_cast walks the class hierarchy's
// RTTI; every pass tests node kinds this way, and dynamic_cast was the type checker's
// largest cost (docs/dev/compile-speed.md).
enum class NodeKind : unsigned char {
    FunctionDecl,
    VarDecl,
    InterfaceDecl,
    StructDecl,
    UnionDecl,
    ExternDecl,
    IntrinsicDecl,
    EnumDecl,
    TypeAliasDecl,
    BlockStmt,
    IfStmt,
    ForStmt,
    ForInStmt,
    WhileStmt,
    DoWhileStmt,
    ReturnStmt,
    BreakStmt,
    ContinueStmt,
    SwitchStmt,
    MatchStmt,
    ThrowStmt,
    TryStmt,
    DeferStmt,
    ThreadJoinStmt,
    AsmStmt,
    ExprStmt,
    BinaryExpr,
    UnaryExpr,
    IncDecExpr,
    QuestionExpr,
    TernaryExpr,
    CallExpr,
    IndexExpr,
    MemberExpr,
    CastExpr,
    LiteralExpr,
    IdentExpr,
    AllocWithExpr,
    TemplateCallExpr,
    StructInitExpr,
    ArrayLitExpr,
    LambdaExpr,
    SizeofExpr,
    ThreadCreateExpr,
    AwaitExpr,
    FreeClosureExpr,
    Program,
};
#define ESKIU_NODE_KIND(K) \
    static constexpr NodeKind kNodeKind = NodeKind::K; \
    NodeKind nodeKind() const override { return kNodeKind; }

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
    virtual NodeKind nodeKind() const = 0;
};

template <class T, class B>
inline T* nodeCast(B* p) {
    return p && p->nodeKind() == std::remove_const_t<T>::kNodeKind ? static_cast<T*>(p) : nullptr;
}

#include "ast_decl.h"
#include "ast_stmt.h"
#include "ast_expr.h"

// ============================================================================
// Program (root node)
// ============================================================================

class Program : public ASTNode {
public:
    ESKIU_NODE_KIND(Program)
    std::vector<DeclPtr> declarations;
    std::vector<std::string> linkLibs;   // `#pragma link("name")`, deduped, in order
    // Each named type written in the primary input (a type spelling's base name, a
    // struct literal's name, a constraint): the editor modes resolve it to its
    // declaration.
    struct TypeRef { int line; int col; std::string name; };
    std::vector<TypeRef> typeRefs;

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
