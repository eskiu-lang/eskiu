#include "parser.h"
#include "../lexer/lexer.h"
#include <stdexcept>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include "parser_internal.h"
#include "../lexer/preprocessor.h"

Parser::Parser(const std::vector<Token>& tok)
    : tokens(tok), current(0) {}

std::string Parser::canonicalPath(const std::string& path) {
    std::error_code ec;
    std::filesystem::path c = std::filesystem::weakly_canonical(std::filesystem::absolute(path, ec), ec);
    return ec ? path : c.string();
}

PackageRoots packageRoots;

static bool isFile(const std::string& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

// The file a package import names inside package root `root` (see resolveImport).
static std::string packageModulePath(const std::string& root, const std::string& name,
                                     const std::string& module) {
    if (!module.empty()) {
        std::string inSrc = root + "/src/" + module + ".esk";
        return isFile(inSrc) ? inSrc : root + "/" + module + ".esk";
    }
    for (const std::string& c : {root + "/" + name + ".esk", root + "/src/" + name + ".esk"})
        if (isFile(c)) return c;
    return root + "/lib.esk";
}

std::string Parser::resolveImport(const std::string& spec, bool isStdlib,
                                  const std::string& basedir, const std::string& stdlibPath,
                                  std::string* err) {
    std::string path = spec;
    if (isStdlib) {
        // A stdlib module, also one in a subdirectory (`<c/unistd>`), comes first; then
        // a package (`<pkg>`, `<pkg/module>`).
        size_t slash = path.find('/');
        path = "stdlib/" + spec + ".esk";
        if (!stdlibPath.empty()) path = stdlibPath + "/" + path;
        std::string head = slash == std::string::npos ? spec : spec.substr(0, slash);
        auto pkg = packageRoots.roots.find(head);
        if (pkg != packageRoots.roots.end() && !isFile(path))
            return packageModulePath(pkg->second, head,
                                     slash == std::string::npos ? "" : spec.substr(slash + 1));
        if (err && !head.empty() && !isFile(path)) {
            if (packageRoots.active)
                *err = "unknown package '" + head + "' (not in eskiu.toml)";
            else if (slash != std::string::npos)
                *err = "unknown module <" + spec + "> (for a file of your own, import \"" + spec + ".esk\")";
        }
        return path;
    }
    if (!basedir.empty() && (path.empty() || path[0] != '/')) return basedir + "/" + path;
    return path;
}

PPImportHook ImportCache::hookFor(const std::string& basedir) {
    return [this, basedir](const std::string& spec, bool isStdlib) {
        if (spec.empty()) return;
        std::string full = Parser::resolveImport(spec, isStdlib, basedir, stdlibPath);
        std::string canon = Parser::canonicalPath(full);
        if (seen.count(canon)) return;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(full, ec)) return;
        std::ifstream file(full);
        if (!file.is_open()) return;
        seen.insert(canon);
        std::ostringstream ss;
        ss << file.rdbuf();
        size_t slash = full.rfind('/');
        PPImportHook sub = hookFor(slash != std::string::npos ? full.substr(0, slash) : ".");
        Entry e;
        preprocess(ss.str(), *macros, e.text, full, e.ppErr, &sub);
        files[canon] = std::move(e);
    };
}

// ============================================================================
// Helper Methods
// ============================================================================

Token Parser::peek() const {
    if (is_at_end()) {
        return tokens.back();
    }
    if (splitActive && current == splitPos) {
        Token t = tokens[current];
        t.type = TokenType::GT;
        t.value = ">";
        return t;
    }
    return tokens[current];
}

Token Parser::peek_ahead(int n) const {
    if (n == 0) return peek();
    size_t pos = current + n;
    if (pos >= tokens.size()) {
        return tokens.back();
    }
    return tokens[pos];
}

Token Parser::advance() {
    if (current < tokens.size()) {
        if (splitActive && current == splitPos) {
            Token t = peek();
            splitActive = false;
            current++;
            return t;
        }
        return tokens[current++];
    }
    if (!tokens.empty()) {
        return tokens.back();
    }
    throw std::runtime_error("No tokens available");
}

bool Parser::check(TokenType type) const {
    if (is_at_end()) return false;
    return peek().type == type;
}

bool Parser::match(TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::match(const std::vector<TokenType>& types) {
    for (TokenType type : types) {
        if (check(type)) {
            advance();
            return true;
        }
    }
    return false;
}

Token Parser::consume(TokenType type, const std::string& message) {
    if (check(type)) {
        return advance();
    }
    // Friendlier diagnostic: when a name is expected but the next token is a
    // reserved keyword (fn/in/match/type names/...), say so at the cause instead
    // of letting it surface far downstream ("Expected ';'", "Expected expression").
    if (type == TokenType::IDENT) {
        TokenType t = peek().type;
        if (t >= TokenType::LET && t <= TokenType::UINT64) {
            const std::string& kw = peek().value;
            fail("expected a name, found keyword '" +
                 (kw.empty() ? tokenTypeToString(t) : kw) + "'");
        }
    }
    fail(message);
}

void Parser::fail(const std::string& message) { fail(message, peek()); }

void Parser::fail(const std::string& message, const Token& at) {
    errLine = at.line;
    errCol = at.column;
    throw ParseError(message);
}

// Does `t` begin a top-level declaration (a type, a qualifier, a decl keyword)?
static bool startsTopLevelDecl(TokenType t) {
    switch (t) {
        case TokenType::IDENT: case TokenType::STRUCT: case TokenType::PACKED:
        case TokenType::UNION: case TokenType::INTERFACE: case TokenType::ENUM:
        case TokenType::EXTERN: case TokenType::INTRINSIC: case TokenType::IMPORT:
        case TokenType::ASYNC: case TokenType::MUST_USE: case TokenType::CONST:
        case TokenType::STATIC: case TokenType::VOLATILE: case TokenType::LET:
        case TokenType::STAR: case TokenType::QUESTION: case TokenType::FN:
        case TokenType::PRAGMA:
            return true;
        default:
            return isPrimitiveTypeToken(t);
    }
}

void Parser::skipToNextDecl(size_t declStart) {
    // The error token may itself start the next declaration (e.g. a missing `}`
    // detected at the following function); resume there rather than skipping it.
    if (current > declStart && !is_at_end() && peek().column == 1 && startsTopLevelDecl(peek().type))
        return;
    if (!is_at_end()) advance();
    while (!is_at_end()) {
        if (peek().column == 1 && startsTopLevelDecl(peek().type)) return;
        advance();
    }
}

bool Parser::is_at_end() const {
    if (current >= tokens.size()) {
        return true;
    }
    // The lexer appends a sentinel EOF token; treat reaching it as end-of-input
    // so the declaration loop does not try to parse EOF as a declaration.
    return tokens[current].type == TokenType::EOF_TOKEN;
}

// ============================================================================
// Type Parsing
// ============================================================================

void Parser::consumeTemplateClose(const char* ctx) {
    if (check(TokenType::GT)) { advance(); return; }
    // A lexed ">>" (right-shift) closes two template levels at once. Its first
    // `>` is consumed by marking the token half-split: until the second `>` is
    // consumed, peek() presents that token as a lone `>`. The token vector is never
    // edited, so a split and its undo (rewindTo) are O(1).
    if (check(TokenType::RSHIFT)) {
        splitActive = true;
        splitPos = current;
        return;
    }
    consume(TokenType::GT, ctx);   // not a close — emit the standard error
}

void Parser::rewindTo(size_t pos) {
    if (splitActive && pos <= splitPos) splitActive = false;
    current = pos;
}

std::string Parser::parseType() {
    std::string type;
    parseTypeInto(type);
    return type;
}

// Append the type at the cursor to `out`. Every prefix (`?`, `const `, leading `*`s)
// is known before the base type is read, so the spelling is built left to right into
// one buffer: nested template arguments and fn types cost time linear in their length.
void Parser::parseTypeInto(std::string& out) {
    TypeLevelGuard guard(*this);

    // Leading `?` marks a checked nullable pointer `?*T`; re-attached as a `?` prefix.
    bool nullable = match(TokenType::QUESTION);

    // Optional leading `const` qualifies the base/pointee: `const int*` is a
    // pointer to const int, spelled with a `const ` prefix. (A `const` before a
    // `let`/decl binding is handled by the declaration parser, not here.)
    bool baseIsConst = match(TokenType::CONST);

    // Handle leading pointers (Rust-style: *i32). A `?` between two stars makes the
    // pointer after it nullable: `*?*T` is a pointer to a nullable pointer to T.
    std::string leading;
    while (check(TokenType::STAR)) {
        enterTypeLevel();
        advance();
        leading += "*";
        if (check(TokenType::QUESTION) && current + 1 < tokens.size() &&
            tokens[current + 1].type == TokenType::STAR) {
            advance();
            leading += "?";
        }
    }

    if (is_at_end()) {
        fail("Unexpected end of file while parsing type");
    }

    Token typeToken = peek();
    // Function pointer type: fn(T,U,...)->R (no `?` prefix, no trailing pointers).
    if (match(TokenType::FN)) {
        if (baseIsConst) out += "const ";
        out += leading;
        out += "fn(";
        consume(TokenType::LPAREN, "Expected '(' in fn type");
        bool first = true;
        while (!check(TokenType::RPAREN) && !is_at_end()) {
            if (!first) { consume(TokenType::COMMA, "Expected ',' between fn parameter types"); out += ","; }
            first = false;
            parseTypeInto(out);
        }
        consume(TokenType::RPAREN, "Expected ')' in fn type");
        out += ")->";
        consume(TokenType::ARROW, "Expected '->' in fn type");
        parseTypeInto(out);
        return;
    }
    if (nullable) out += "?";
    if (baseIsConst) out += "const ";
    out += leading;
    if (isPrimitiveTypeToken(typeToken.type)) {
        advance();
        out += typeToken.value;
    } else if (check(TokenType::IDENT)) {
        out += advance().value;
        // Template instantiation: Name<TypeArg, ...>  e.g. Result<int, string>
        if (match(TokenType::LT)) {
            out += "<";
            bool first = true;
            do {
                if (!first) out += ",";
                first = false;
                parseTypeInto(out);
            } while (match(TokenType::COMMA));
            consumeTemplateClose("Expected '>' after template arguments");
            out += ">";
        }
    } else {
        fail("Expected type, got " + tokenTypeToString(peek().type));
    }

    // Handle trailing pointers (C-style: i32*). A `const` right after a star
    // makes that pointer level const (`int* const`), encoded as `*const`.
    while (check(TokenType::STAR)) {
        enterTypeLevel();
        advance();
        out += "*";
        if (match(TokenType::CONST)) out += "const";
    }

    // Handle array syntax [N] — capture the size literal
    while (check(TokenType::LBRACKET)) {
        enterTypeLevel();
        advance();
        std::string sizeStr;
        while (!is_at_end() && !check(TokenType::RBRACKET)) {
            sizeStr += peek().value;
            advance();
        }
        if (!match(TokenType::RBRACKET)) {
            fail("Expected ']'");
        }
        out += "[" + sizeStr + "]";
    }
}

std::vector<std::pair<std::string, std::string>> Parser::parseParameterList(
        std::vector<bool>* escaping, std::vector<std::pair<int, int>>* positions,
        std::vector<ExprPtr>* defaults, std::vector<std::string>* defaultTexts) {
    std::vector<std::pair<std::string, std::string>> params;

    if (!check(TokenType::RPAREN)) {
        do {
            // Handle variadic parameters (...)
            if (match(TokenType::ELLIPSIS)) {
                params.push_back({"...", "..."});
                if (escaping) escaping->push_back(false);
                if (positions) positions->push_back({tokens[current - 1].line, tokens[current - 1].column});
                break;
            }

            // Optional `escaping` qualifier: the parameter retains the closure
            // beyond the call (e.g. stores it), so closures passed here need a
            // heap environment.
            bool isEscaping = match(TokenType::ESCAPING);
            std::string type = parseType();
            Token nameTok = consume(TokenType::IDENT, "Expected parameter name");
            params.push_back({type, nameTok.value});
            if (escaping) escaping->push_back(isEscaping);
            if (positions) positions->push_back({nameTok.line, nameTok.column});
            ExprPtr def;
            std::string defText;
            if (check(TokenType::EQ)) {
                Token eq = advance();
                if (!defaults) fail("a default value is allowed only on a function's parameter", eq);
                Token from = peek();
                def = parseTernary();
                defText = sourceSpan(from, peek());
            }
            if (defaults) { defaults->push_back(def); defaultTexts->push_back(defText); }
        } while (match(TokenType::COMMA));
    }

    return params;
}

// ============================================================================
// Declarations
// ============================================================================

// The source text from token `from` up to (not including) token `to`, with each run of
// whitespace (newlines too) folded to one space; "" without a source.
std::string Parser::sourceSpan(const Token& from, const Token& to) const {
    if (!sourceText) return "";
    auto offsetOf = [&](const Token& t) -> size_t {
        size_t off = 0; int line = 1;
        while (line < t.line && off < sourceText->size()) { if ((*sourceText)[off] == '\n') line++; off++; }
        return std::min(sourceText->size(), off + (size_t)std::max(0, t.column - 1));
    };
    size_t a = offsetOf(from), b = offsetOf(to);
    std::string out;
    bool space = false;
    for (size_t i = a; i < b; ++i) {
        char c = (*sourceText)[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { space = !out.empty(); continue; }
        if (space) { out += ' '; space = false; }
        out += c;
    }
    return out;
}

std::shared_ptr<Program> Parser::parse() {
    auto decls = parseProgram();
    if (hadError) return nullptr;
    auto prog = std::make_shared<Program>(decls);
    prog->linkLibs = linkLibs;
    return prog;
}

std::vector<DeclPtr> Parser::parseProgram() {
    std::vector<DeclPtr> declarations;

    // Owned import set if caller didn't provide one
    std::set<std::string> ownedSet;
    if (!importedFiles) importedFiles = &ownedSet;
    // The root parser owns the shared type-name set; sub-parsers point at it.
    if (!sharedTypeNames) sharedTypeNames = &declaredTypeNames;
    if (!sharedGenericNames) sharedGenericNames = &declaredGenericNames;

    // Panic-mode recovery shared by both parse paths: report the error at its
    // location, then skip to the next top-level declaration so one bad
    // declaration neither aborts the file nor cascades into spurious errors.
    size_t declStart = 0;
    auto recover = [&](const std::exception& e) {
        std::cerr << "error: " << (filename.empty() ? "<input>" : filename) << ":"
                  << errLine << ":" << errCol << ": " << e.what() << std::endl;
        if (firstErrorLine() == 0) { firstErrorLine() = errLine; firstErrorCol() = errCol; firstErrorFile() = filename; }
        hadError = true;
        skipToNextDecl(declStart);
    };

    while (!is_at_end()) {
        declStart = current;
        // Compiler directive (#pragma pack / link): updates parser state, emits no decl.
        if (check(TokenType::PRAGMA)) {
            Token pt = advance();
            try { applyPragma(pt); } catch (const std::exception& e) { recover(e); }
            continue;
        }
        // Handle import "path/to/file.esk"  or  import <stdlib_name>
        if (match(TokenType::IMPORT)) {
            try {
                std::string spec;
                bool isStdlib = false;

                Token pathTok = peek();
                if (check(TokenType::STRING_LIT)) {
                    // import "relative/path.esk"
                    spec = advance().value;
                } else if (check(TokenType::LT)) {
                    // import <name>  →  resolved against stdlibPath
                    advance(); // consume <
                    while (!check(TokenType::GT) && !is_at_end())
                        spec += advance().value;
                    consume(TokenType::GT, "Expected '>' after stdlib name");
                    isStdlib = true;
                } else {
                    fail("Expected filename or <name> after import");
                }
                consume(TokenType::SEMICOLON, "Expected ';' after import");

                std::string pkgErr;
                std::string fullPath = resolveImport(spec, isStdlib, basedir, stdlibPath, &pkgErr);
                if (!pkgErr.empty()) fail(pkgErr, pathTok);
                std::string canon = canonicalPath(fullPath);
                if (!importedFiles->count(canon)) {
                    importedFiles->insert(canon);

                    std::error_code ec;
                    if (std::filesystem::is_directory(fullPath, ec))
                        fail("Cannot open import: '" + fullPath + "'", pathTok);
                    // The preprocessor already handled this import at its line (see
                    // ImportCache); otherwise read and preprocess it now.
                    auto cached = importCache ? importCache->files.find(canon)
                                              : std::map<std::string, ImportCache::Entry>::iterator();
                    bool haveCached = importCache && cached != importCache->files.end();
                    std::string src;
                    if (!haveCached) {
                        std::ifstream file(fullPath);
                        if (!file.is_open())
                            fail("Cannot open import: '" + fullPath + "'", pathTok);
                        std::ostringstream ss;
                        ss << file.rdbuf();
                        src = ss.str();
                    }
                    size_t slash = fullPath.rfind('/');
                    std::string subdir = (slash != std::string::npos) ? fullPath.substr(0, slash) : ".";
                    PPImportHook hook;
                    if (importCache) hook = importCache->hookFor(subdir);
                    Lexer lexer = haveCached
                        ? Lexer::fromPreprocessed(cached->second.text, fullPath, cached->second.ppErr)
                        : Lexer(src, macros, fullPath, importCache ? &hook : nullptr);
                    std::vector<Token> itoks;
                    Token t = lexer.next_token();
                    while (t.type != TokenType::EOF_TOKEN) { itoks.push_back(t); t = lexer.next_token(); }
                    itoks.push_back(t);
                    // A lexical error in the import stops here, like one in a root
                    // file: parsing the broken token stream would only cascade.
                    if (lexer.hadError) { hadError = true; continue; }

                    Parser sub(itoks);
                    std::string subSource = lexer.text();
                    sub.sourceText    = &subSource;
                    sub.filename      = fullPath;
                    sub.isImport      = true;
                    sub.basedir       = subdir;
                    sub.stdlibPath    = stdlibPath;
                    sub.importedFiles = importedFiles;
                    sub.macros        = macros;
                    sub.importCache   = importCache;
                    sub.sharedTypeNames = sharedTypeNames;   // one set for all parsers
                    sub.sharedGenericNames = sharedGenericNames;

                    auto subProg = sub.parse();
                    if (!subProg) {
                        hadError = true;
                    } else {
                        for (const auto& d : subProg->declarations) if (d) d->fromImport = true;
                        declarations.insert(declarations.end(),
                            subProg->declarations.begin(), subProg->declarations.end());
                        for (const auto& l : subProg->linkLibs) addLinkLib(l);
                        // Type names are recorded directly into the shared set as
                        // each file is parsed, so a cast to an imported type —
                        // `(FutureHdr*)p` — parses correctly here regardless of
                        // which import path defined it (no post-merge needed).
                    }
                }
            } catch (const std::exception& e) {
                recover(e);
            }
            continue;
        }

        try {
            if (startsScriptStatement()) {
                Token at = peek();
                if (isImport) fail("a top-level statement is allowed only in a program's own file, not in an import", at);
                scriptOrder.push_back({parseStatement(), nullptr});
                continue;
            }
            DeclPtr decl = parseDeclaration();
            if (decl) {
                decl->sourceFile = filename;
                declarations.push_back(decl);
                if (auto* vd = dynamic_cast<VarDecl*>(decl.get())) scriptOrder.push_back({nullptr, vd});
            }
        } catch (const std::exception& e) {
            recover(e);
        }
    }

    try { buildScriptMain(declarations); } catch (const std::exception& e) { recover(e); }
    return declarations;
}

// A top-level line is a statement when no declaration can start that way: a statement
// keyword, `{`, a literal or prefix operator, a call `f(`, an assignment, member or
// postfix use of a name (`x = 1`, `p.x`, `arr[0] = 2` but not `Point[2] ps`), a label,
// or a write through a pointer (`*p = 1`, but not `*Point p`). A rule on tokens only, so
// both compilers read the same file the same way.
bool Parser::startsScriptStatement() const {
    auto isStmtAfterName = [](TokenType t) {
        switch (t) {
            case TokenType::LPAREN: case TokenType::DOT: case TokenType::EQ: case TokenType::PLUS_EQ:
            case TokenType::MINUS_EQ: case TokenType::STAR_EQ: case TokenType::SLASH_EQ:
            case TokenType::PERCENT_EQ: case TokenType::AMP_EQ: case TokenType::PIPE_EQ:
            case TokenType::CARET_EQ: case TokenType::LSHIFT_EQ: case TokenType::RSHIFT_EQ:
            case TokenType::PLUS_PLUS: case TokenType::MINUS_MINUS: case TokenType::COLON:
            case TokenType::SEMICOLON:
                return true;
            default:
                return false;
        }
    };
    TokenType t = peek().type;
    switch (t) {
        case TokenType::IF: case TokenType::FOR: case TokenType::WHILE: case TokenType::DO:
        case TokenType::RETURN: case TokenType::SWITCH: case TokenType::MATCH: case TokenType::TRY:
        case TokenType::THROW: case TokenType::DEFER: case TokenType::ERRDEFER: case TokenType::BREAK:
        case TokenType::CONTINUE: case TokenType::LBRACE: case TokenType::PLUS_PLUS:
        case TokenType::MINUS_MINUS: case TokenType::LPAREN: case TokenType::INT_LIT:
        case TokenType::FLOAT_LIT: case TokenType::STRING_LIT: case TokenType::CHAR_LIT:
        case TokenType::MINUS: case TokenType::NOT: case TokenType::AMPERSAND: case TokenType::TILDE:
        case TokenType::NULL_KW: case TokenType::TRUE: case TokenType::FALSE: case TokenType::SIZEOF:
        case TokenType::FREE_CLOSURE: case TokenType::THREAD_CREATE: case TokenType::THREAD_JOIN:
        case TokenType::ALLOC_WITH: case TokenType::AWAIT: case TokenType::ASM: case TokenType::SEMICOLON:
            return true;
        case TokenType::IDENT:
        case TokenType::STAR: {
            if (t == TokenType::IDENT && peek().value == "deprecated" &&
                peek_ahead(1).type == TokenType::LPAREN && peek_ahead(2).type == TokenType::STRING_LIT)
                return false;
            int k = 0;
            while (peek_ahead(k).type == TokenType::STAR) ++k;
            if (peek_ahead(k).type != TokenType::IDENT) return false;
            TokenType n = peek_ahead(k + 1).type;
            if (isStmtAfterName(n)) return true;
            if (n == TokenType::LT && k == 0) {
                // `f<int>(x)` is a call and `x < y` (no closing `>`) a comparison;
                // `List<int> l` declares.
                int depth = 0, j = 1;
                for (;; ++j) {
                    TokenType b = peek_ahead(j).type;
                    if (b == TokenType::SEMICOLON || b == TokenType::LBRACE || b == TokenType::RBRACE ||
                        b == TokenType::EOF_TOKEN)
                        return true;
                    if (b == TokenType::LT) depth++;
                    else if (b == TokenType::GT) depth--;
                    else if (b == TokenType::RSHIFT) depth -= 2;
                    if (depth <= 0) break;
                }
                return depth < 0 || peek_ahead(j + 1).type == TokenType::LPAREN;
            }
            if (n != TokenType::LBRACKET) return false;
            // `arr[0] = 1` uses a name; `Point[2] ps` and `*Node[3] t` declare one.
            int depth = 0, j = k + 1;
            for (;; ++j) {
                TokenType b = peek_ahead(j).type;
                if (b == TokenType::EOF_TOKEN) return false;
                if (b == TokenType::LBRACKET) depth++;
                else if (b == TokenType::RBRACKET && --depth == 0) break;
            }
            return peek_ahead(j + 1).type != TokenType::IDENT;
        }
        default:
            return false;
    }
}

// A file with top-level statements is a script: it gets `int main(int __argc, string*
// __argv)` running them in order, with the arguments in the globals `argc` and `argv`.
// Each top-level variable with an initializer (not `const`, not an array literal)
// becomes a global that starts at zero and is assigned at its place in that order, so
// its initializer may be any expression and the functions see it.
void Parser::buildScriptMain(std::vector<DeclPtr>& decls) {
    StmtPtr first;
    for (const auto& e : scriptOrder) if (e.first) { first = e.first; break; }
    if (!first) return;
    for (const auto& d : decls)
        if (auto* fd = dynamic_cast<FunctionDecl*>(d.get()); fd && fd->name == "main" && !fd->fromImport) {
            errLine = first->line;
            errCol = first->col;
            throw ParseError("a file with top-level statements has no 'main': the statements are its main");
        }
    std::vector<BlockItem> items;
    // `main`, `argc`, `argv`, their assignments and the final `return 0` are implicit:
    // no position (line 0), so the editor modes do not place them on the first statement.
    for (const char* name : {"argc", "argv"}) {
        auto be = std::make_shared<BinaryExpr>(std::make_shared<IdentExpr>(name), "=",
                                               std::make_shared<IdentExpr>(std::string("__") + name));
        items.push_back(StmtPtr(std::make_shared<ExprStmt>(be)));
    }
    for (const auto& [stmt, vd] : scriptOrder) {
        if (stmt) {
            items.push_back(stmt);
            continue;
        }
        if (!vd->initializer || vd->isConst || vd->isExtern || dynamic_cast<ArrayLitExpr*>(vd->initializer.get()))
            continue;
        vd->scriptInit = true;
        auto lhs = std::make_shared<IdentExpr>(vd->name);
        lhs->line = vd->line;
        lhs->col = vd->col;
        auto be = std::make_shared<BinaryExpr>(lhs, "=", vd->initializer);
        be->line = vd->line;
        be->col = vd->col;
        be->scriptInit = true;
        auto es = std::make_shared<ExprStmt>(be);
        es->line = vd->line;
        es->col = vd->col;
        items.push_back(StmtPtr(es));
    }
    bool returns = !items.empty() && std::holds_alternative<StmtPtr>(items.back()) &&
                   dynamic_cast<ReturnStmt*>(std::get<StmtPtr>(items.back()).get());
    if (!returns) items.push_back(StmtPtr(std::make_shared<ReturnStmt>(std::make_shared<LiteralExpr>(LiteralExpr::Kind::INT, "0"))));
    auto body = std::make_shared<BlockStmt>(items);
    body->line = first->line;
    body->col = first->col;
    auto mainFn = std::make_shared<FunctionDecl>(
        "main", "int", std::vector<std::pair<std::string, std::string>>{{"int", "__argc"}, {"string*", "__argv"}}, body);
    mainFn->sourceFile = filename;
    auto ga = std::make_shared<VarDecl>("argc", "int", nullptr);
    auto gv = std::make_shared<VarDecl>("argv", "string*", nullptr);
    ga->sourceFile = gv->sourceFile = filename;
    decls.insert(decls.begin(), {ga, gv});
    decls.push_back(mainFn);
}
