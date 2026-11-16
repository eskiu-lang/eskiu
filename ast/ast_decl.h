// ast_decl.h: the declaration nodes (part of ast.h, which includes it in order: include ast.h, not this file).
#pragma once

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
    ESKIU_NODE_KIND(FunctionDecl)
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
    // `export`: the function is a C entry point, defined with the target's C calling
    // convention (structs and unions by value as C passes them) under its own name.
    bool isExport = false;
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
    ESKIU_NODE_KIND(VarDecl)
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
    ESKIU_NODE_KIND(InterfaceDecl)
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
    ESKIU_NODE_KIND(StructDecl)
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
    ESKIU_NODE_KIND(UnionDecl)
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
    ESKIU_NODE_KIND(ExternDecl)
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
    ESKIU_NODE_KIND(IntrinsicDecl)
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
    ESKIU_NODE_KIND(EnumDecl)
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
    ESKIU_NODE_KIND(TypeAliasDecl)
    std::string aliased; // the underlying type string

    TypeAliasDecl(const std::string& name, std::string aliased)
        : Decl(name), aliased(std::move(aliased)) {}

    void accept(class ASTVisitor* visitor) override;
};
