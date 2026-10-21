#pragma once

#include <memory>
#include <stdexcept>
#include <set>
#include <string>
#include <vector>
#include "../lexer/lexer.h"
#include "../ast/ast.h"

// The import prescan shared by one compilation. The preprocessor reports each import
// at its line (PPImportHook); hookFor() preprocesses the imported file right there,
// with the macro table as it stands at that point, and keeps the text here until the
// parser reaches the import. So a #define before `import "x.esk"` reaches x.esk, one
// after it does not, and x.esk's own #defines reach the importer's later lines (C's
// textual #include order).
// Package imports: the dependencies listed in eskiu.lock, name -> package root
// directory. `active` when the program being compiled belongs to a package (an
// eskiu.toml in its directory or above), so an import naming no stdlib module and no
// dependency is "unknown package". The driver fills it (pkgLoadRoots in pkg.cpp)
// before parsing; resolveImport reads it.
struct PackageRoots {
    bool active = false;
    std::map<std::string, std::string> roots;
};
extern PackageRoots packageRoots;

struct ImportCache {
    std::string stdlibPath;
    std::map<std::string, Macro>* macros = nullptr;
    struct Entry { std::string text; bool ppErr = false; };
    std::map<std::string, Entry> files;   // canonical path → preprocessed text
    std::set<std::string> seen;           // canonical paths preprocessed (incl. roots)
    // The hook for a file whose relative imports resolve against `basedir`.
    PPImportHook hookFor(const std::string& basedir);
};

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens);
    // The (preprocessed) source the tokens came from, for `dbg(e)`'s text; may be null.
    const std::string* sourceText = nullptr;

    // Parse the token stream. Returns nullptr (and prints diagnostics to stderr)
    // if any declaration failed to parse.
    std::shared_ptr<Program> parse();

    // Set true when any declaration/import failed to parse. parse() returns
    // nullptr in that case so callers fail loudly instead of silently dropping code.
    bool hadError = false;
    // The first syntax error of the run (file and line), for editor tooling that
    // retries without the broken line (--complete-at).
    static std::string& firstErrorFile() { static std::string f; return f; }
    static int& firstErrorLine() { static int l = 0; return l; }
    static int& firstErrorCol() { static int c = 0; return c; }

    // Directory of the current source file — used to resolve relative imports
    std::string basedir;
    // The source file being parsed; labels diagnostics ("<input>" when empty).
    std::string filename;
    // Set on the parser of an imported file: a top-level statement is an error there.
    bool isImport = false;
    // Root of the Eskiu installation — used to resolve <stdlib> imports
    // Set from $ESKIU_ROOT env var or dirname(argv[0])/../lib/eskiu
    std::string stdlibPath;
    // Shared set of already-imported canonical paths (prevents re-importing). The
    // driver registers each root input too, so a file importing its importer (or
    // a diamond reaching one file by two spellings) parses it once.
    std::set<std::string>* importedFiles = nullptr;
    // The key importedFiles uses: an absolute path with `.`/`..`/symlinks resolved
    // (the path itself when it cannot be resolved).
    static std::string canonicalPath(const std::string& path);
    // Shared preprocessor macro table — lets #defines propagate into imports
    std::map<std::string, Macro>* macros = nullptr;
    // Shared import prescan (see ImportCache); null → an import is preprocessed when
    // the parser reaches it.
    ImportCache* importCache = nullptr;
    // Where an import resolves: `<name>` (no '/') is stdlib/name.esk under the stdlib
    // root, a relative "path" is under `basedir`. `<pkg/module>` is module.esk in the
    // root of dependency `pkg` (src/module.esk when that exists), and `<pkg>` alone, when
    // no stdlib module has that name, is pkg.esk, src/pkg.esk or lib.esk there. `err`
    // receives "unknown package" for a `<name...>` that names neither inside a package.
    static std::string resolveImport(const std::string& spec, bool isStdlib,
                                     const std::string& basedir, const std::string& stdlibPath,
                                     std::string* err = nullptr);
    // Shared across all sub-parsers (like importedFiles): type names declared in
    // ANY file, so a cast to a type stays a cast even when that type's defining
    // import was deduplicated via a different path. Without sharing, a file that
    // imports an already-imported module never learned its type names and
    // misparsed `(Type*)x` casts (e.g. `(Future<T>*)0` after `import <future>`).
    std::set<std::string>* sharedTypeNames = nullptr;
    // Shared the same way: names of generic functions and generic enum variants, the
    // callees whose `Name<...>(` is a template call even when a type argument is a
    // bare name not yet declared (see parsePostfix).
    std::set<std::string>* sharedGenericNames = nullptr;

private:
    // Backing store for sharedTypeNames in the root parser; sub-parsers point
    // sharedTypeNames at the root's. Names of declared types (structs, enums,
    // unions, aliases) — lets the cast parser recognize (TypeName)expr.
    std::set<std::string> declaredTypeNames;
    std::set<std::string> declaredGenericNames;   // backing store for sharedGenericNames
    // Consume a template-closing '>'. Handles a lexed '>>' (right-shift) at the
    // close of nested templates (List<List<int>>) by splitting it: the inner
    // close turns '>>' into a single '>' left for the outer close.
    void consumeTemplateClose(const char* ctx);
    std::vector<Token> tokens;
    std::string sourceSpan(const Token& from, const Token& to) const;
    size_t current;
    // Safety net against pathological input: nested expressions/statements beyond
    // kMaxNesting levels are rejected with an error instead of overflowing the stack
    // here or in the later passes, which recurse once per level. The compiler runs on
    // a large stack (see main.cpp), so the limit sits far above realistic code. Long
    // operator chains and statement lists are loops and do not count.
    static constexpr int kMaxNesting = 100000;
    int nesting = 0;
    // The nesting error has its own type so a speculative parse (which catches and
    // discards ordinary syntax errors to try another reading) rethrows it: past the
    // limit no alternative reading is valid, and retrying at every level would bury
    // the real diagnostic under a misleading one.
    struct NestingError : std::runtime_error { using std::runtime_error::runtime_error; };
    void enterNesting() {
        if (++nesting > kMaxNesting) {
            --nesting;
            Token at = peek();
            errLine = at.line;
            errCol = at.column;
            throw NestingError("nesting too deep (more than " + std::to_string(kMaxNesting) + " levels)");
        }
    }
    // Types nest far less: every template argument list, fn type, pointer level and
    // array dimension is one level, and at most kMaxTypeNesting are allowed. Later
    // passes spell a generic instance by its whole argument list, so the cost of a
    // type grows faster than its depth; the limit (like C++'s template depth) keeps
    // every type cheap. Raised as a NestingError too.
    static constexpr int kMaxTypeNesting = 1000;
    int typeNesting = 0;
    void enterTypeLevel() {
        if (++typeNesting > kMaxTypeNesting) {
            Token at = peek();
            errLine = at.line;
            errCol = at.column;
            throw NestingError("type nesting too deep (more than " + std::to_string(kMaxTypeNesting) + " levels)");
        }
    }
    // One level for the type being parsed; restores the depth when the type ends.
    struct TypeLevelGuard {
        Parser& p;
        int saved;
        explicit TypeLevelGuard(Parser& parser) : p(parser), saved(parser.typeNesting) { p.enterTypeLevel(); }
        ~TypeLevelGuard() { p.typeNesting = saved; }
    };
    struct NestGuard {
        Parser& p;
        explicit NestGuard(Parser& parser) : p(parser) { p.enterNesting(); }
        ~NestGuard() { --p.nesting; }
    };
    // A `>>` whose first `>` consumeTemplateClose has consumed: while splitActive,
    // the token at splitPos reads as a single `>` (the second half). A speculative
    // parse that backtracks past it restores the whole `>>`, so a later
    // `x < y >> 1` still sees the shift it really is.
    bool splitActive = false;
    size_t splitPos = 0;
    // Backtrack to a saved position, undoing a `>>` split made at or after it.
    void rewindTo(size_t pos);

    // ParseError represents syntax errors reported by `fail()`. A speculative parse
    // catches ParseError to rewind and try an alternative reading, while letting
    // other exceptions (NestingError, std::bad_alloc, internal errors) propagate.
    struct ParseError : std::runtime_error { using std::runtime_error::runtime_error; };

    // Speculatively execute `fn()`. If `fn()` throws NestingError, it rethrows immediately.
    // If it throws ParseError, rewinds to `savePos` and returns a default-constructed result.
    template <typename F>
    auto speculate(F&& fn) -> decltype(fn()) {
        size_t savePos = current;
        try {
            return fn();
        } catch (const NestingError&) {
            throw;
        } catch (const ParseError&) {
            rewindTo(savePos);
            return decltype(fn()){};
        }
    }

    // Speculatively execute `fn()` with conditional backtracking: only backtracks if
    // `shouldBacktrack(startingToken)` is true; otherwise rethrows the ParseError to surface
    // the accurate error diagnostic.
    template <typename F, typename ShouldBacktrack>
    auto speculateIf(F&& fn, ShouldBacktrack&& shouldBacktrack) -> decltype(fn()) {
        size_t savePos = current;
        try {
            return fn();
        } catch (const NestingError&) {
            throw;
        } catch (const ParseError&) {
            if (!shouldBacktrack(tokens[savePos].type)) throw;
            rewindTo(savePos);
            return decltype(fn()){};
        }
    }

    // Helper methods
    Token peek() const;
    Token peek_ahead(int n = 1) const;
    Token advance();
    bool check(TokenType type) const;
    bool match(TokenType type);
    bool match(const std::vector<TokenType>& types);
    Token consume(TokenType type, const std::string& message);
    // Report a syntax error at the current token (or at `at`): records its position
    // for the diagnostic, then throws. Speculative parses catch and discard it.
    [[noreturn]] void fail(const std::string& message);
    [[noreturn]] void fail(const std::string& message, const Token& at);
    int errLine = 0, errCol = 0;
    // Panic-mode recovery: skip to the next token that plausibly starts a top-level
    // declaration (at column 1), so one error does not cascade.
    void skipToNextDecl(size_t declStart);
    bool is_at_end() const;

    // Parsing methods
    std::vector<DeclPtr> parseProgram();

    DeclPtr parseDeclaration();
    DeclPtr parseFunctionDecl();
    std::string parseOperatorToken();   // reads the op after `operator` → "+", "[]", "u-", ...
    DeclPtr parseStructDecl();
    StructDecl::Pad parseUnnamedBitfield(const std::string& type, size_t before);
    DeclPtr parseExternDecl();
    DeclPtr parseIntrinsicDecl();

    // #pragma pack state: structs declared while currentPack==1 are packed.
    int currentPack = 0;
    std::vector<int> packStack;
    void applyPragma(const Token& tok);
    // `#pragma link("name")` libraries, in first-seen order without duplicates
    // (this file's plus its imports'); the driver links each as -l<name>.
    std::vector<std::string> linkLibs;
    void addLinkLib(const std::string& name);

    StmtPtr parseStatement();
    // Scripts: a file with top-level statements gets an implicit `main` (see
    // buildScriptMain). The statements and the top-level variables, in order.
    bool startsScriptStatement() const;
    std::vector<std::pair<StmtPtr, VarDecl*>> scriptOrder;
    void buildScriptMain(std::vector<DeclPtr>& decls);
    StmtPtr parseBlockStatement();
    BlockItem parseBlockItem();
    StmtPtr parseIfStatement();
    StmtPtr parseForStatement();
    StmtPtr parseWhileStatement();
    StmtPtr parseDoWhileStatement();
    StmtPtr parseReturnStatement();
    StmtPtr parseBreakStatement();
    StmtPtr parseContinueStatement();
    StmtPtr parseSwitchStatement();
    StmtPtr parseMatchStatement();
    // When set, a bare `Name {` is not parsed as a struct literal (so a match
    // subject's trailing `{` opens the match body). Restored after the subject.
    bool noStructLiteral = false;
    // Does a lambda whose return type is not a type keyword start here
    // (`S() {`, `*S(int k) {`, `Box<int>(T x) {`)? A token scan, no parse.
    bool lambdaAhead() const;
    ExprPtr tryParseLambda();
    // `trailing`: a call statement may end in a block, passed as its last argument (a
    // `void()` lambda). Only the statement path allows it, not a `for` header.
    StmtPtr parseExpressionStatement(bool trailing = false);
    // A `{` right after a call's ')' where no trailing block is allowed: a located error.
    void rejectTrailingBlock();
    // The index of the ')' that closed the last call parsed, for the trailing block rule.
    size_t lastCallClose = static_cast<size_t>(-1);
    // The first token of a statement that may end in a trailing block: there `Name(...) {`
    // is a call with a block, not a lambda returning `Name` (whose value a statement drops).
    size_t trailingStart = static_cast<size_t>(-1);

    ExprPtr parseStructInit(const std::string& structName);
    ExprPtr parseExpression();
    ExprPtr parseAssignment();
    ExprPtr parseTernary();
    bool ternaryColonAhead() const;   // disambiguate `cond ? a : b` from postfix `expr?`
    mutable std::vector<char> colonAhead;   // ternaryColonAhead's answer per start index
    // Binary operators by precedence climbing: parse a unary operand, then fold every
    // operator binding at least `minPrec` (same-precedence operators in a loop, so a
    // long `a + b + c ...` chain costs no recursion; a tighter operator on the right
    // recurses at most once per precedence level).
    ExprPtr parseBinary(int minPrec);
    // Parse an optional `<T, U: Iface + Other>` type-parameter list (shared by fn and
    // struct decls); fills the params and per-param constraints, a no-op with no '<'.
    void parseTypeParams(std::vector<std::string>& typeParams,
                         std::map<std::string, std::vector<std::string>>& typeConstraints);
    ExprPtr parseUnary();
    // At `(`: does a `*`-led parenthesized form open a cast? `(*T)x` is a cast, but
    // `(*p)`, `(*p) - 1`, `(*p)++` and `(*sp).a` dereference a variable.
    bool starParenIsCast() const;
    // Type parameters of the generic function/struct being parsed: `(*T)x` inside
    // `alloc<T>` names a type even though T is not a declared type name.
    std::vector<std::string> typeParamScope;
    bool isTypeName(const std::string& name) const;
    // Locals and parameters in scope, innermost last. A local named like a type
    // (`int P = 10;` beside `struct P`) shadows it, so `(P) - 1` is not a cast.
    std::vector<std::string> localVars;
    bool isLocalVar(const std::string& name) const;
    // Drops the names a block, function or lambda body declared when it ends.
    struct LocalScope {
        Parser& p;
        size_t mark;
        explicit LocalScope(Parser& parser) : p(parser), mark(parser.localVars.size()) {}
        ~LocalScope() { p.localVars.resize(mark); }
    };
    void declareParams(const std::vector<std::pair<std::string, std::string>>& params) {
        for (const auto& pr : params) localVars.push_back(pr.second);
    }
    // Can the parsed template argument `t` only be a type? A bare name that is not a
    // declared type (`b` in `f(a < b, c > (d))`) may equally be a comparison operand.
    bool typeArgIsEvident(const std::string& t) const;
    ExprPtr parsePostfix();
    ExprPtr parsePrimary();

    std::string parseType();
    void parseTypeInto(std::string& out);
    std::vector<std::pair<std::string, std::string>> parseParameterList(
        std::vector<bool>* escaping = nullptr,
        std::vector<std::pair<int, int>>* positions = nullptr,
        std::vector<ExprPtr>* defaults = nullptr, std::vector<std::string>* defaultTexts = nullptr);
};
