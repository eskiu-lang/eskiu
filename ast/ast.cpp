#include "ast.h"

// Declarations
void FunctionDecl::accept(ASTVisitor* visitor) { visitor->visit(this); }
void VarDecl::accept(ASTVisitor* visitor) { visitor->visit(this); }
void StructDecl::accept(ASTVisitor* visitor) { visitor->visit(this); }
void ExternDecl::accept(ASTVisitor* visitor) { visitor->visit(this); }
void IntrinsicDecl::accept(ASTVisitor* visitor) { visitor->visit(this); }

// Statements
void BlockStmt::accept(ASTVisitor* visitor) { visitor->visit(this); }
void IfStmt::accept(ASTVisitor* visitor) { visitor->visit(this); }
void ForStmt::accept(ASTVisitor* visitor) { visitor->visit(this); }
void ForInStmt::accept(ASTVisitor* visitor) { visitor->visit(this); }
void WhileStmt::accept(ASTVisitor* visitor) { visitor->visit(this); }
void DoWhileStmt::accept(ASTVisitor* visitor) { visitor->visit(this); }
void ReturnStmt::accept(ASTVisitor* visitor) { visitor->visit(this); }
void BreakStmt::accept(ASTVisitor* visitor) { visitor->visit(this); }
void ExprStmt::accept(ASTVisitor* visitor) { visitor->visit(this); }

// Expressions
void BinaryExpr::accept(ASTVisitor* visitor) { visitor->visit(this); }

BinaryExpr::~BinaryExpr() {
    // Take each solely owned left operand off its node before that node dies, so the
    // spine is released one node at a time instead of by nested destructor calls.
    ExprPtr next = std::move(left);
    while (next && next.use_count() == 1) {
        auto* b = nodeCast<BinaryExpr>(next.get());
        if (!b) break;
        ExprPtr after = std::move(b->left);
        next = std::move(after);
    }
}
void UnaryExpr::accept(ASTVisitor* visitor) { visitor->visit(this); }
void IncDecExpr::accept(ASTVisitor* visitor) { visitor->visit(this); }
void QuestionExpr::accept(ASTVisitor* visitor) { visitor->visit(this); }
void TernaryExpr::accept(ASTVisitor* visitor) { visitor->visit(this); }
void CallExpr::accept(ASTVisitor* visitor) { visitor->visit(this); }
void IndexExpr::accept(ASTVisitor* visitor) { visitor->visit(this); }
void MemberExpr::accept(ASTVisitor* visitor) { visitor->visit(this); }
void CastExpr::accept(ASTVisitor* visitor) { visitor->visit(this); }
void LiteralExpr::accept(ASTVisitor* visitor) { visitor->visit(this); }
void IdentExpr::accept(ASTVisitor* visitor) { visitor->visit(this); }
void AllocWithExpr::accept(ASTVisitor* visitor) { visitor->visit(this); }
void InterfaceDecl::accept(ASTVisitor* visitor) { visitor->visit(this); }
void ContinueStmt::accept(ASTVisitor* visitor) { visitor->visit(this); }
void SwitchStmt::accept(ASTVisitor* visitor) { visitor->visit(this); }
void MatchStmt::accept(ASTVisitor* visitor) { visitor->visit(this); }
void TemplateCallExpr::accept(ASTVisitor* visitor) { visitor->visit(this); }
void StructInitExpr::accept(ASTVisitor* visitor) { visitor->visit(this); }
void ArrayLitExpr::accept(ASTVisitor* visitor) { visitor->visit(this); }

void LambdaExpr::accept(ASTVisitor* visitor) { visitor->visit(this); }
void AsmStmt::accept(ASTVisitor* visitor)        { visitor->visit(this); }
void UnionDecl::accept(ASTVisitor* v)            { v->visit(this); }
void SizeofExpr::accept(ASTVisitor* v)           { v->visit(this); }
void FreeClosureExpr::accept(ASTVisitor* v)      { v->visit(this); }
void AwaitExpr::accept(ASTVisitor* v)            { v->visit(this); }
void ThreadCreateExpr::accept(ASTVisitor* v)     { v->visit(this); }
void ThreadJoinStmt::accept(ASTVisitor* v)       { v->visit(this); }
void ThrowStmt::accept(ASTVisitor* v)            { v->visit(this); }
void TryStmt::accept(ASTVisitor* v)              { v->visit(this); }
void DeferStmt::accept(ASTVisitor* v)            { v->visit(this); }
void EnumDecl::accept(ASTVisitor* v)             { v->visit(this); }
void TypeAliasDecl::accept(ASTVisitor* v)        { v->visit(this); }

// Program
void Program::accept(ASTVisitor* visitor) { visitor->visit(this); }
