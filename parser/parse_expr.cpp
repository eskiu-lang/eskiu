#include "parser.h"
#include "../lexer/lexer.h"
#include <stdexcept>
#include "parser_internal.h"

// Parser — expression parsing: the precedence ladder, unary/postfix/primary,
// and struct-init literals.
// Part of the parser.cpp split; see parser.h.

ExprPtr Parser::parseStructInit(const std::string& structName) {
    Token lbTok = consume(TokenType::LBRACE, "Expected '{'");
    std::vector<std::pair<std::string, ExprPtr>> inits;
    std::vector<std::pair<int, int>> positions;

    if (!check(TokenType::RBRACE)) {
        do {
            // Named: fieldName: expr
            if (check(TokenType::IDENT) && peek_ahead(1).type == TokenType::COLON) {
                Token fieldTok = advance();
                advance(); // consume ':'
                positions.push_back({fieldTok.line, fieldTok.column});
                inits.push_back({fieldTok.value, parseExpression()});
            } else {
                // Positional
                positions.push_back({0, 0});
                inits.push_back({"", parseExpression()});
            }
        } while (match(TokenType::COMMA) && !check(TokenType::RBRACE));   // trailing comma ok
    }

    consume(TokenType::RBRACE, "Expected '}'");
    auto si = std::make_shared<StructInitExpr>(structName, std::move(inits));
    si->fieldPositions = std::move(positions);
    si->line = lbTok.line; si->col = lbTok.column;
    return si;
}

ExprPtr Parser::parseExpression() {
    return parseAssignment();
}

// True if the `?` at `current` opens a ternary — a matching `:` follows at the same
// bracket nesting before a statement/argument terminator — rather than the postfix
// Result-propagation operator (`expr?`). Propagation `?` is never followed by a
// same-level `:`, so the colon reliably signals a ternary.
//
// The answer for every start position is computed once, right to left (a group is
// skipped by jumping to its closing bracket), so nested ternaries cost linear time
// rather than a rescan per `?`.
bool Parser::ternaryColonAhead() const {
    if (colonAhead.empty()) {
        size_t n = tokens.size();
        auto isOpen = [](TokenType t) {
            return t == TokenType::LPAREN || t == TokenType::LBRACKET || t == TokenType::LBRACE;
        };
        auto isClose = [](TokenType t) {
            return t == TokenType::RPAREN || t == TokenType::RBRACKET || t == TokenType::RBRACE;
        };
        std::vector<size_t> closer(n, n), open;
        for (size_t i = 0; i < n; ++i) {
            if (isOpen(tokens[i].type)) open.push_back(i);
            else if (isClose(tokens[i].type) && !open.empty()) { closer[open.back()] = i; open.pop_back(); }
        }
        colonAhead.assign(n + 1, 0);
        for (size_t i = n; i-- > 0;) {
            TokenType t = tokens[i].type;
            if (isOpen(t)) colonAhead[i] = closer[i] < n ? colonAhead[closer[i] + 1] : 0;
            else if (isClose(t)) colonAhead[i] = 0;   // closed the enclosing group before any ':'
            else if (t == TokenType::COLON) colonAhead[i] = 1;
            else if (t == TokenType::SEMICOLON || t == TokenType::COMMA || t == TokenType::EOF_TOKEN) colonAhead[i] = 0;
            else colonAhead[i] = colonAhead[i + 1];
        }
    }
    return current + 1 < colonAhead.size() && colonAhead[current + 1];
}

ExprPtr Parser::parseTernary() {
    NestGuard guard(*this);
    ExprPtr cond = parseBinary(1);
    if (check(TokenType::QUESTION) && ternaryColonAhead()) {
        Token qTok = advance();                       // consume '?'
        ExprPtr thenE = parseAssignment();            // then-arm: a full expression
        consume(TokenType::COLON, "Expected ':' in ternary expression");
        ExprPtr elseE = parseTernary();               // else-arm: right-associative
        return withPos(std::make_shared<TernaryExpr>(cond, thenE, elseE), qTok);
    }
    return cond;
}

ExprPtr Parser::parseAssignment() {
    NestGuard guard(*this);
    ExprPtr expr = parseTernary();

    // Compound assignments: desugar x += y  →  x = x + y
    static const std::unordered_map<TokenType, std::string> compound = {
        {TokenType::PLUS_EQ,    "+"},  {TokenType::MINUS_EQ,   "-"},
        {TokenType::STAR_EQ,    "*"},  {TokenType::SLASH_EQ,   "/"},
        {TokenType::PERCENT_EQ, "%"},
        {TokenType::AMP_EQ,     "&"},  {TokenType::PIPE_EQ,    "|"},
        {TokenType::CARET_EQ,   "^"},  {TokenType::LSHIFT_EQ,  "<<"},
        {TokenType::RSHIFT_EQ,  ">>"},
    };
    for (auto& [tt, op] : compound) {
        if (match(tt)) {
            Token opTok = tokens[current - 1];
            ExprPtr rhs  = parseAssignment();
            auto binOp   = withPos(std::make_shared<BinaryExpr>(expr, op,  rhs),  opTok);
            return withPos(std::make_shared<BinaryExpr>(expr, "=", binOp), opTok);
        }
    }

    if (match(TokenType::EQ)) {
        Token opTok = tokens[current - 1];
        ExprPtr value = parseAssignment();
        return withPos(std::make_shared<BinaryExpr>(expr, "=", value), opTok);
    }

    return expr;
}

// Binding strength of a binary operator token, loosest first (0: not a binary
// operator): || < && < | < ^ < & < == != < relational < shifts < + - < * / %.
static int binaryPrecedence(TokenType t) {
    switch (t) {
        case TokenType::OR:        return 1;
        case TokenType::AND:       return 2;
        case TokenType::PIPE:      return 3;
        case TokenType::CARET:     return 4;
        case TokenType::AMPERSAND: return 5;
        case TokenType::EQEQ: case TokenType::NE: return 6;
        case TokenType::LT: case TokenType::GT: case TokenType::LE: case TokenType::GE: return 7;
        case TokenType::LSHIFT: case TokenType::RSHIFT: return 8;
        case TokenType::PLUS: case TokenType::MINUS: return 9;
        case TokenType::STAR: case TokenType::SLASH: case TokenType::PERCENT: return 10;
        default: return 0;
    }
}

ExprPtr Parser::parseBinary(int minPrec) {
    ExprPtr expr = parseUnary();
    for (;;) {
        int prec = is_at_end() ? 0 : binaryPrecedence(peek().type);
        if (prec == 0 || prec < minPrec) return expr;
        Token opTok = advance();
        ExprPtr rhs = parseBinary(prec + 1);
        expr = withPos(std::make_shared<BinaryExpr>(expr, opTok.value, rhs), opTok);
    }
}

bool Parser::isTypeName(const std::string& name) const {
    if (sharedTypeNames && sharedTypeNames->count(name)) return true;
    for (const auto& tp : typeParamScope) if (tp == name) return true;
    return false;
}

bool Parser::isLocalVar(const std::string& name) const {
    for (const auto& v : localVars) if (v == name) return true;
    return false;
}

bool Parser::typeArgIsEvident(const std::string& t) const {
    if (t.find_first_of("<(") != std::string::npos) return true;   // Name<...> or fn(...)->R
    size_t b = 0, e = t.size();
    for (;;) {
        if (b < e && (t[b] == '?' || t[b] == '*')) b++;
        else if (t.compare(b, 6, "const ") == 0) b += 6;
        else break;
    }
    for (;;) {
        if (e > b && t[e - 1] == ']') { size_t o = t.rfind('[', e - 1); if (o == std::string::npos || o < b) break; e = o; }
        else if (e - b > 6 && t.compare(e - 6, 6, "*const") == 0) e -= 6;
        else if (e > b && t[e - 1] == '*') e--;
        else break;
    }
    std::string base = t.substr(b, e - b);
    static const std::set<std::string> prims = {
        "int", "int8", "int16", "int32", "int64", "uint", "uint8", "uint16", "uint32", "uint64",
        "float", "double", "bool", "char", "string", "void"};
    return prims.count(base) > 0 || isTypeName(base);
}

bool Parser::starParenIsCast() const {
    size_t k = 1;
    while (peek_ahead(k).type == TokenType::STAR) k++;
    // Only `( *... IDENT )` is ambiguous; `(*int)`, `(*Foo<T>)`, `(*fn(...)->R)` are types.
    if (peek_ahead(k).type != TokenType::IDENT || peek_ahead(k + 1).type != TokenType::RPAREN)
        return true;
    if (isTypeName(peek_ahead(k).value)) return true;
    // An unknown name: a cast only when an operand follows the `)`. A binary operator,
    // `++`/`--`, `.`, `[`, `=`, a call's `(` (`(*pf)(x)`) or a terminator means `(*p)` is
    // a dereference.
    switch (peek_ahead(k + 2).type) {
        case TokenType::IDENT: case TokenType::INT_LIT: case TokenType::FLOAT_LIT:
        case TokenType::STRING_LIT: case TokenType::CHAR_LIT: case TokenType::TRUE:
        case TokenType::FALSE: case TokenType::NULL_KW:
        case TokenType::NOT: case TokenType::TILDE: case TokenType::SIZEOF:
            return true;
        default:
            return false;
    }
}

bool Parser::lambdaAhead() const {
    size_t i = current, n = tokens.size();
    auto at = [&](size_t k) { return k < n ? tokens[k].type : TokenType::EOF_TOKEN; };
    if (at(i) == TokenType::QUESTION) i++;
    while (at(i) == TokenType::STAR) i++;
    if (at(i) != TokenType::IDENT && !isPrimitiveTypeToken(at(i))) return false;
    i++;
    if (at(i) == TokenType::LT) {
        int d = 1;
        for (i++; i < n && d > 0; i++) {
            TokenType t = at(i);
            if (t == TokenType::LT) d++;
            else if (t == TokenType::GT) d--;
            else if (t == TokenType::RSHIFT) d -= 2;
            else if (t != TokenType::IDENT && !isPrimitiveTypeToken(t) && t != TokenType::COMMA &&
                     t != TokenType::STAR && t != TokenType::QUESTION) return false;
        }
        if (d != 0) return false;
    }
    while (at(i) == TokenType::STAR) i++;
    if (at(i) != TokenType::LPAREN) return false;
    int d = 0;
    for (; i < n; i++) {
        TokenType t = at(i);
        if (t == TokenType::LPAREN) d++;
        else if (t == TokenType::RPAREN) { if (--d == 0) break; }
        else if (t == TokenType::LBRACE || t == TokenType::SEMICOLON || t == TokenType::EOF_TOKEN) return false;
    }
    return at(i + 1) == TokenType::LBRACE;
}

// Lambda: RetType(params) { body }. Speculative: backs out (returns null) when the
// tokens do not form one. Only the header is a guess: once `RetType(params) {` is read the
// body is parsed for good, so an error in it is reported where it is, not as a failed guess
// at the lambda (the self-hosted parser does the same).
ExprPtr Parser::tryParseLambda() {
    Token tok = peek();
    std::string retType;
    std::vector<bool> esc;
    std::vector<std::pair<std::string, std::string>> params;
    bool header = speculate([&]() -> bool {
        retType = parseType();
        consume(TokenType::LPAREN, "");
        params = parseParameterList(&esc);
        consume(TokenType::RPAREN, "");
        if (!check(TokenType::LBRACE)) throw ParseError("not a lambda");
        return true;
    });
    if (!header) return nullptr;
    LocalScope scope(*this);
    declareParams(params);
    StmtPtr body = parseBlockStatement();
    auto lambda = std::make_shared<LambdaExpr>(params, retType, body);
    lambda->line = tok.line; lambda->col = tok.column;
    lambda->paramEscaping = esc;
    return lambda;
}

ExprPtr Parser::parseUnary() {
    NestGuard guard(*this);
    // A lambda whose return type is a struct, pointer or generic type. Not in a
    // match subject, where `f() {` is the call and the match body.
    if ((!noStructLiteral || isPrimitiveTypeToken(peek().type)) && lambdaAhead()) {
        if (ExprPtr l = tryParseLambda()) return l;
    }
    // await E — prefix operator; binds like a unary operator.
    if (check(TokenType::AWAIT)) {
        Token awaitTok = advance();
        ExprPtr operand = parseUnary();
        return withPos(std::make_shared<AwaitExpr>(operand), awaitTok);
    }
    // Fold -N and -N.N into a negative literal directly (avoids UnaryExpr for constants)
    if (check(TokenType::MINUS)) {
        TokenType next = peek_ahead(1).type;
        if (next == TokenType::INT_LIT || next == TokenType::FLOAT_LIT) {
            Token minusTok = advance(); // consume '-'
            Token numTok   = advance(); // consume number
            if (numTok.type == TokenType::INT_LIT)
                return withPos(std::make_shared<LiteralExpr>(
                    LiteralExpr::Kind::INT, "-" + numTok.value), minusTok);
            else
                return withPos(std::make_shared<LiteralExpr>(
                    LiteralExpr::Kind::FLOAT, "-" + numTok.value), minusTok);
        }
    }

    // Prefix ++x / --x
    if (match({TokenType::PLUS_PLUS, TokenType::MINUS_MINUS})) {
        Token opTok = tokens[current - 1];
        bool dec = opTok.type == TokenType::MINUS_MINUS;
        ExprPtr operand = parseUnary();
        return withPos(std::make_shared<IncDecExpr>(operand, dec, /*prefix=*/true), opTok);
    }

    if (match({TokenType::NOT, TokenType::MINUS, TokenType::PLUS, TokenType::AMPERSAND, TokenType::STAR, TokenType::TILDE})) {
        Token opToken = tokens[current - 1];
        ExprPtr expr = parseUnary();
        return withPos(std::make_shared<UnaryExpr>(opToken.value, expr), opToken);
    }

    // Cast expression: (TYPE) expr
    // Only trigger on unambiguous type keywords to avoid conflict with (expr).
    if (check(TokenType::LPAREN)) {
        TokenType inner = peek_ahead(1).type;
        bool isTypeKeyword = isPrimitiveTypeToken(inner) ||
                             (inner == TokenType::STAR && starParenIsCast());
        // Also a cast when the inner token names a declared type — a struct,
        // enum, union, or alias — as `(Name)x`, `(Name*)x`, or `(Name<...>)x`,
        // unless a local of that name shadows the type.
        if (!isTypeKeyword && inner == TokenType::IDENT &&
            isTypeName(peek_ahead(1).value) && !isLocalVar(peek_ahead(1).value)) {
            isTypeKeyword = true;
        }
        if (isTypeKeyword) {
            ExprPtr castExpr = speculate([&]() -> ExprPtr {
                Token lpTok = advance(); // consume (
                std::string castType = parseType();
                if (match(TokenType::RPAREN)) {
                    ExprPtr expr = parseUnary();
                    return withPos(std::make_shared<CastExpr>(castType, expr), lpTok);
                }
                throw ParseError("not a cast");
            });
            if (castExpr) return castExpr;
        }
    }

    return parsePostfix();
}

ExprPtr Parser::parsePostfix() {
    ExprPtr expr = parsePrimary();

    while (true) {
        // Template function call: ident<TypeArg, ...>(args)
        // `a < b, c > (d)` reads as a call only when the callee is a known generic
        // (function, enum variant or type) or every argument can only be a type.
        if (auto* ident = dynamic_cast<IdentExpr*>(expr.get())) {
            if (check(TokenType::LT)) {
                ExprPtr templExpr = speculate([&]() -> ExprPtr {
                    advance(); // consume <
                    std::vector<std::string> typeArgs;
                    do { typeArgs.push_back(parseType()); } while (match(TokenType::COMMA));
                    bool knownGeneric = sharedGenericNames->count(ident->name) || isTypeName(ident->name);
                    bool allTypes = true;
                    for (const auto& ta : typeArgs) allTypes = allTypes && typeArgIsEvident(ta);
                    if ((knownGeneric || allTypes) && (check(TokenType::GT) || check(TokenType::RSHIFT))) {
                        consumeTemplateClose("Expected '>'");
                        if (match(TokenType::LPAREN)) {
                            // Template function call: Name<T,...>(args)
                            std::vector<ExprPtr> args;
                            if (!check(TokenType::RPAREN)) {
                                do { args.push_back(parseExpression()); } while (match(TokenType::COMMA));
                            }
                            consume(TokenType::RPAREN, "Expected ')'");
                            auto tc = std::make_shared<TemplateCallExpr>(ident->name, typeArgs, std::move(args));
                            tc->line = ident->line; tc->col = ident->col;
                            return tc;
                        }
                        if (check(TokenType::LBRACE)) {
                            // Template struct literal: Name<T,...> { ... }
                            std::string typeStr = ident->name + "<";
                            for (size_t i = 0; i < typeArgs.size(); ++i) {
                                if (i) typeStr += ",";
                                typeStr += typeArgs[i];
                            }
                            typeStr += ">";
                            return parseStructInit(typeStr);
                        }
                    }
                    throw ParseError("not a template call");
                });
                if (templExpr) {
                    expr = templExpr;
                    continue;
                }
            }
        }
        if (match(TokenType::LPAREN)) {
            Token callTok = tokens[current - 1];
            Token firstArg = tokens[current];
            std::vector<ExprPtr> args;
            if (!check(TokenType::RPAREN)) {
                do { args.push_back(parseExpression()); } while (match(TokenType::COMMA));
            }
            Token closeTok = tokens[current];
            consume(TokenType::RPAREN, "Expected ')'");
            auto call = withPos(std::make_shared<CallExpr>(expr, args), callTok);
            if (auto* id = dynamic_cast<IdentExpr*>(expr.get()); id && id->name == "dbg" && args.size() == 1)
                call->dbgText = sourceSpan(firstArg, closeTok);
            expr = call;
        } else if (match(TokenType::LBRACKET)) {
            Token idxTok = tokens[current - 1];
            ExprPtr index = parseExpression();
            // `base[lo..hi]` is a slice expression (half-open); `base[i]` a plain index.
            ExprPtr highIndex = nullptr;
            if (match(TokenType::RANGE)) highIndex = parseExpression();
            consume(TokenType::RBRACKET, "Expected ']'");
            expr = withPos(std::make_shared<IndexExpr>(expr, index, highIndex), idxTok);
        } else if (match(TokenType::DOT)) {
            Token dotTok = tokens[current - 1];
            Token memTok = static_cast<int>(peek().type) <= static_cast<int>(TokenType::UINT64)
                ? advance() : consume(TokenType::IDENT, "Expected member name");
            auto mem = withPos(std::make_shared<MemberExpr>(expr, memTok.value), dotTok);
            mem->memberLine = memTok.line; mem->memberCol = memTok.column;
            expr = mem;
        } else if (check(TokenType::QUESTION) && !ternaryColonAhead()) {
            // Postfix Result-propagation `expr?` — but only when this `?` does not open
            // a ternary (no same-level `:` ahead); the ternary is handled lower down.
            Token qTok = advance();
            expr = withPos(std::make_shared<QuestionExpr>(expr), qTok);
        } else if (match({TokenType::PLUS_PLUS, TokenType::MINUS_MINUS})) {
            Token pTok = tokens[current - 1];
            bool dec = pTok.type == TokenType::MINUS_MINUS;
            expr = withPos(std::make_shared<IncDecExpr>(expr, dec, /*prefix=*/false), pTok);
        } else {
            break;
        }
    }

    return expr;
}

ExprPtr Parser::parsePrimary() {
    Token tok = peek();

    // Array literal `{ e0, e1, ... }` (untyped; target-typed at the declaration).
    if (check(TokenType::LBRACE)) {
        advance();  // '{'
        std::vector<ExprPtr> elems;
        if (!check(TokenType::RBRACE)) {
            elems.push_back(parseExpression());
            while (match(TokenType::COMMA)) {
                if (check(TokenType::RBRACE)) break;   // trailing comma
                elems.push_back(parseExpression());
            }
        }
        consume(TokenType::RBRACE, "Expected '}' after array literal");
        return withPos(std::make_shared<ArrayLitExpr>(std::move(elems)), tok);
    }

    if (match(TokenType::TRUE)) {
        return withPos(std::make_shared<LiteralExpr>(LiteralExpr::Kind::BOOL, "true"), tok);
    }
    if (match(TokenType::FALSE)) {
        return withPos(std::make_shared<LiteralExpr>(LiteralExpr::Kind::BOOL, "false"), tok);
    }
    if (match(TokenType::NULL_KW)) {
        return withPos(std::make_shared<LiteralExpr>(LiteralExpr::Kind::NULL_VAL, "null"), tok);
    }
    if (match(TokenType::INT_LIT)) {
        return withPos(std::make_shared<LiteralExpr>(LiteralExpr::Kind::INT, tok.value), tok);
    }
    if (match(TokenType::FLOAT_LIT)) {
        return withPos(std::make_shared<LiteralExpr>(LiteralExpr::Kind::FLOAT, tok.value), tok);
    }
    if (match(TokenType::STRING_LIT)) {
        // Adjacent string literal concatenation: "abc" "def" → "abcdef"
        std::string combined = tok.value;
        while (check(TokenType::STRING_LIT)) {
            combined += peek().value;
            advance();
        }
        return withPos(std::make_shared<LiteralExpr>(LiteralExpr::Kind::STRING, combined), tok);
    }
    if (match(TokenType::CHAR_LIT)) {
        return withPos(std::make_shared<LiteralExpr>(LiteralExpr::Kind::CHAR, tok.value), tok);
    }
    // alloc_with(&allocator, T, N) — like alloc, but from an explicit allocator
    if (match(TokenType::ALLOC_WITH)) {
        consume(TokenType::LPAREN, "Expected '(' after alloc_with");
        ExprPtr allocator = parseExpression();
        consume(TokenType::COMMA, "Expected ',' after allocator in alloc_with");
        std::string elemType = parseType();
        consume(TokenType::COMMA, "Expected ',' after type in alloc_with");
        ExprPtr count = parseExpression();
        consume(TokenType::RPAREN, "Expected ')'");
        return withPos(std::make_shared<AllocWithExpr>(allocator, elemType, count), tok);
    }


    // sizeof(T) or sizeof(expr) -> int64. The operand is a type when it reads as one
    // up to the `)`: a bare name (a type or a variable, told apart by sema) or a
    // composite spelling over a known type (`*Node`, `int[4]`). Anything else
    // (`*p`, `a[0]`, `p.x`, `x + 1`) is an expression, measured by its type and not
    // evaluated.
    if (match(TokenType::SIZEOF)) {
        consume(TokenType::LPAREN, "Expected '(' after sizeof");
        std::string typeName;
        bool asType = speculate([&]() -> bool {
            typeName = parseType();
            if (check(TokenType::RPAREN) &&
                (typeName.find_first_of("*[?") == std::string::npos || typeArgIsEvident(typeName))) {
                return true;
            }
            throw ParseError("not a sizeof type");
        });
        if (!asType) {
            ExprPtr operand = parseExpression();
            consume(TokenType::RPAREN, "Expected ')'");
            auto sz = withPos(std::make_shared<SizeofExpr>(""), tok);
            sz->operand = operand;
            return sz;
        }
        consume(TokenType::RPAREN, "Expected ')'");
        return withPos(std::make_shared<SizeofExpr>(typeName), tok);
    }

    // free_closure(closureExpr) -> void — release an escaping closure's env.
    if (match(TokenType::FREE_CLOSURE)) {
        consume(TokenType::LPAREN, "Expected '(' after free_closure");
        ExprPtr c = parseExpression();
        consume(TokenType::RPAREN, "Expected ')'");
        return withPos(std::make_shared<FreeClosureExpr>(c), tok);
    }

    // thread_create(fn()->void worker) -> *void
    if (match(TokenType::THREAD_CREATE)) {
        consume(TokenType::LPAREN, "Expected '(' after thread_create");
        ExprPtr worker = parseExpression();
        consume(TokenType::RPAREN, "Expected ')'");
        return withPos(std::make_shared<ThreadCreateExpr>(worker), tok);
    }

    // Lambda: int(int a, int b) { return a + b; }
    // Detected when a type keyword is followed by '(' and the content looks like params + '{'
    {
        bool isTypeKw = isPrimitiveTypeToken(tok.type);
        if (isTypeKw && peek_ahead(1).type == TokenType::LPAREN) {
            // Disambiguate from a cast-like usage: try to parse as lambda, backtrack on failure
            if (ExprPtr l = tryParseLambda()) return l;
        }
    }

    if (match(TokenType::IDENT)) {
        if (check(TokenType::LBRACE) && !noStructLiteral) {
            return withPos(parseStructInit(tok.value), tok);
        }
        return withPos(std::make_shared<IdentExpr>(tok.value), tok);
    }
    if (match(TokenType::LPAREN)) {
        ExprPtr expr = parseExpression();
        if (!match(TokenType::RPAREN)) {
            fail("Expected ')'");
        }
        return expr;
    }

    fail(std::string("Expected expression, got ") + tokenTypeToString(tok.type));
}
