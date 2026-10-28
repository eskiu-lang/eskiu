#pragma once

#include "eir.h"
#include "../ast/ast.h"
#include "../sema/type.h"
#include <map>
#include <vector>
#include <string>
#include <memory>

namespace eir {

class EIRBuilder : public ASTVisitor {
public:
    EIRBuilder();
    ~EIRBuilder() override = default;

    // Single-resolver tables from TypeChecker (post-AsyncTransform)
    const std::map<Expr*, std::string>* resolvedExprTypes = nullptr;
    const std::map<std::string, std::pair<std::string, std::vector<std::string>>>* semaInstanceArgs = nullptr;
    // Each generic instance's expression types (TypeChecker::instanceExprTypeMap).
    const std::map<Expr*, std::vector<std::pair<std::map<std::string, std::string>, std::string>>>* instanceExprTypes = nullptr;
    std::string targetTriple;

    // Build EIR Module from AST Program
    std::unique_ptr<Module> build(Program* program);

    // Convert Eskiu type string to EIR Type
    TypePtr toEirType(const std::string& typeStr);

    // Visitor methods
    void visit(Program* node) override;
    void visit(FunctionDecl* node) override;
    void visit(VarDecl* node) override;
    void visit(StructDecl* node) override;
    void visit(ExternDecl* node) override;
    void visit(IntrinsicDecl* node) override;
    void visit(InterfaceDecl* node) override;
    void visit(EnumDecl* node) override;
    void visit(TypeAliasDecl* node) override;
    void visit(BlockStmt* node) override;
    void visit(IfStmt* node) override;
    void visit(ForStmt* node) override;
    void visit(ForInStmt* node) override;
    void visit(WhileStmt* node) override;
    void visit(DoWhileStmt* node) override;
    void visit(ReturnStmt* node) override;
    void visit(BreakStmt* node) override;
    void visit(ContinueStmt* node) override;
    void visit(SwitchStmt* node) override;
    void visit(MatchStmt* node) override;
    void visit(ExprStmt* node) override;
    void visit(BinaryExpr* node) override;
    void visit(UnaryExpr* node) override;
    void visit(IncDecExpr* node) override;
    void visit(QuestionExpr* node) override;
    void visit(TernaryExpr* node) override;
    void visit(CallExpr* node) override;
    void visit(IndexExpr* node) override;
    void visit(MemberExpr* node) override;
    void visit(CastExpr* node) override;
    void visit(LiteralExpr* node) override;
    void visit(IdentExpr* node) override;
    void visit(StructInitExpr* node) override;
    void visit(ArrayLitExpr* node) override;
    void visit(AllocWithExpr* node) override;
    void visit(TemplateCallExpr* node) override;
    void visit(LambdaExpr* node) override;
    void visit(AsmStmt* node) override;
    void visit(UnionDecl* node) override;
    void visit(SizeofExpr* node) override;
    void visit(ThreadCreateExpr* node) override;
    void visit(FreeClosureExpr* node) override;
    void visit(AwaitExpr* node) override;
    void visit(ThreadJoinStmt* node) override;
    void visit(ThrowStmt* node) override;
    void visit(TryStmt* node) override;
    void visit(DeferStmt* node) override;

private:
    std::unique_ptr<Module> module;
    Function* currentFunction = nullptr;
    BasicBlock* currentBlock = nullptr;

    int nextVarId = 0;
    int nextBlockId = 0;

    std::string newVar();
    BasicBlock* createBlock(const std::string& prefix = "bb");
    void setBlock(BasicBlock* bb);
    void emit(Instruction inst);

    // Scoping for local variables (%name -> pointer operand)
    std::vector<std::map<std::string, Operand>> scopeStack;
    std::vector<std::map<std::string, TypePtr>> varTypeStack;
    void pushScope();
    void popScope();
    void declareVar(const std::string& name, Operand allocaSlot, TypePtr type);
    Operand* lookupVar(const std::string& name);
    TypePtr lookupVarType(const std::string& name);

    // Loop target stacks for break and continue
    std::vector<BasicBlock*> breakStack;
    std::vector<BasicBlock*> continueStack;

    // Expression evaluation
    Operand evaluateExpr(const ExprPtr& expr);
    Operand evaluateLValue(const ExprPtr& expr);
    Operand lastValue;
    Operand lastLValue;

    TypePtr getExprEirType(const ExprPtr& expr);
    TypePtr getExprEirType(const Expr* expr);

    struct AdtVariantInfo {
        std::string enumName;
        int variantIndex = 0;
        std::vector<TypePtr> payloadTypes;
    };
    std::unordered_map<std::string, AdtVariantInfo> adtVariants;
    std::unordered_map<std::string, EnumDecl*> plainEnums;
    std::unordered_map<std::string, EnumDecl*> adtEnums;
    std::unordered_map<std::string, int64_t> enumConstants;
    std::unordered_map<std::string, TypePtr> constTypes;   // a `const` global's type (enum members are i32)
    struct InterfaceInfo {
        std::string name;
        std::vector<std::string> methodNames;
        std::vector<std::vector<std::string>> methodParamTypes;
        std::vector<std::string> methodReturnTypes;
    };
    std::unordered_map<std::string, InterfaceInfo> interfaces;
    Operand boxAsInterface(const std::string& ifaceName, const std::string& structName, const Operand& structPtr);
    Operand evalForType(const ExprPtr& e, const std::string& targetType);
    Operand evalForType(const ExprPtr& e, TypePtr targetType);
    Operand closureTarget(const Operand& fn);
    bool isGlobalName(const std::string& name) const;
    std::string instanceOperator(const std::string& op, const std::vector<Operand>& ops);
    Operand callOperator(const std::string& fn, const std::vector<Operand>& args);
    std::string eskiuSpelling(const TypePtr& t);
    std::string semaType(const Expr* e);
    Operand selectPtr(const Operand& c, const Operand& a, const Operand& b);
    void dbgFormat(const std::string& t, const Operand& addr, std::string& fmt, std::vector<Operand>& args, int depth);
    Operand emitDbg(CallExpr* node);
    std::vector<std::string> catchTextSlots;   // each enclosing catch's exception_text buffer
    Operand coerceNumeric(const Operand& v, const TypePtr& to);
    Operand promoteInt(const Operand& v);
    Operand stepByOne(const Operand& cur, const TypePtr& ty, bool decrement);
    Operand truth(const Operand& v);
    Operand compoundOp(const std::string& baseOp, const Operand& cur, const Operand& rhs, const TypePtr& curTy);
    static TypePtr commonType(const TypePtr& a, const TypePtr& b);
    static int64_t truncConst(int64_t v, const TypePtr& t);
    static int64_t saturateToInt(double f, const TypePtr& t);
    Operand cFnPointer(const ExprPtr& e);

    // Template tracking & instantiation
    std::map<std::string, std::string> typeAliases;
    std::map<std::string, StructDecl*> templateDecls;
    std::map<std::string, FunctionDecl*> funcTemplateDecls;
    std::map<std::string, std::pair<std::string, std::vector<std::string>>> templateInstanceArgs;
    std::map<std::string, std::string> typeParamOverride;

    void unifyTypeParam(std::string pattern, std::string concrete,
                        const std::set<std::string>& tps,
                        std::map<std::string, std::string>& subs) const;
    Function* instantiateGenericMethod(const std::string& instName, const std::string& method);
    Function* instantiateFnTemplate(FunctionDecl* fd, const std::string& mangledName,
                                    const std::map<std::string, std::string>& subs);
    FunctionDecl* genericFreeMethod(const std::string& instName, const std::string& method) const;

    // Volatile tracking
    std::set<std::string> volatileVars;
    bool volatileRooted(const Expr* e) const;

    // Defer & cleanup stack
    struct DeferEntry {
        StmtPtr body;
        bool isErr = false;
        std::vector<std::map<std::string, Operand>> scopes;   // the names visible at the defer
        std::vector<std::map<std::string, TypePtr>> varTypes;
    };
    void visitBody(const StmtPtr& body);
    void entryAlloca(const std::string& name, const TypePtr& type, size_t align);
    void emitZeroFill(const Operand& ptr, const TypePtr& type);
    std::map<Function*, std::set<std::string>> functionSlots;   // each function's variable slots
    std::vector<std::vector<DeferEntry>> cleanupScopes;
    void runCleanupsToDepth(size_t depth, bool errorPath = false);

    // Loop context with cleanup depth
    struct LoopContext {
        BasicBlock* breakTarget = nullptr;
        BasicBlock* continueTarget = nullptr;
        size_t cleanupDepth = 0;
        size_t handlerDepth = 0;
        std::string label;   // `outer: for ...`; "" for a switch or an unlabeled loop
    };
    const LoopContext* jumpTarget(const std::string& label, bool isContinue) const;
    void jumpTo(const LoopContext* loop, bool isContinue);
    std::vector<LoopContext> loopContextStack;

    // Exception handling
    BasicBlock* currentUnwindTarget = nullptr;
    std::vector<BasicBlock*> unwindTargetStack;
    size_t currentHandlerDepth = 0;
    void emitDeferPad(Stmt* body);
    std::string exceptionTypeName(const std::string& raw) const;

    // Bitfield helpers
    void storeBitfield(MemberExpr* m, Operand val, bool isVolatile);
    Operand loadBitfield(MemberExpr* m, bool isVolatile);
    const StructFieldInfo* bitfieldOf(MemberExpr* m);
    TypePtr bitfieldReadType(const StructFieldInfo* fi);
    void emitCompoundAssign(BinaryExpr* node, BinaryExpr* rhsOp);
    Operand bitfieldBase(MemberExpr* m);
    Operand loadBitfieldAt(const Operand& base, const StructFieldInfo* fi, bool isVolatile);
    void storeBitfieldAt(const Operand& base, const StructFieldInfo* fi, Operand val, bool isVolatile);
    void layoutRecord(const std::string& name, const std::vector<StructDecl::Field>& fields,
                      const std::vector<StructDecl::Pad>& pads, bool isUnion, int packAlign,
                      const std::map<std::string, std::string>& subs);
    void ensureStructLayout(const std::string& name);
    void ensureLayoutOf(const TypePtr& t);
    ExprPtr defaultExprFor(const std::string& t);
    std::shared_ptr<StructInitExpr> withDefaults(StructInitExpr* init);
    std::map<std::string, int> structHasDefaults;
    int forInCounter = 0;
    void layoutAdt(const std::string& name, EnumDecl* e, const std::map<std::string, std::string>& subs);
    const AdtVariantInfo* variantFor(const std::string& variant, const std::string& instName);
    std::string genericVariantInstance(const std::string& variant, const std::vector<std::string>& args);
    std::unordered_map<std::string, EnumDecl*> genericEnums;
    std::unordered_map<std::string, AdtVariantInfo> adtInstanceVariants;   // "Inst::Variant" of a generic enum instance
    std::unordered_map<std::string, StructDecl*> plainStructs;
    std::unordered_map<std::string, UnionDecl*> unionDecls;

    int nextLambdaId = 0;
    int64_t foldConstInt(const ExprPtr& expr);
    bool foldCInt(const ExprPtr& expr, ty::CInt& out);
    uint64_t sizeOfType(const std::string& spelling);
    void emitArrayInitInto(Operand dest, ArrayLitExpr* lit, TypePtr arrTy);
};

} // namespace eir
