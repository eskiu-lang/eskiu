#include "parser.h"
#include "../lexer/lexer.h"
#include <stdexcept>
#include "parser_internal.h"
#include "../ast/ast_walk.h"

// Parser — statement parsing (blocks, control flow, match/switch, returns).
// Part of the parser.cpp split; see parser.h.

// ============================================================================
// Statements
// ============================================================================

StmtPtr Parser::parseStatement() {
    NestGuard guard(*this);
    // Labeled loop: IDENT ':' <for|while|do>. A bare `IDENT:` at statement level has no
    // other meaning, and we only treat it as a label when a loop keyword follows.
    if (check(TokenType::IDENT) && peek_ahead(1).type == TokenType::COLON) {
        TokenType after = peek_ahead(2).type;
        if (after == TokenType::FOR || after == TokenType::WHILE || after == TokenType::DO) {
            std::string label = advance().value;   // IDENT
            advance();                              // ':'
            StmtPtr loop;
            if (check(TokenType::FOR))        loop = parseForStatement();
            else if (check(TokenType::WHILE)) loop = parseWhileStatement();
            else                              loop = parseDoWhileStatement();
            // parseForStatement yields a ForStmt or a ForInStmt; stamp the label on whichever.
            if (auto* f = dynamic_cast<ForStmt*>(loop.get()))          f->label = label;
            else if (auto* fi = dynamic_cast<ForInStmt*>(loop.get()))  fi->label = label;
            else if (auto* w = dynamic_cast<WhileStmt*>(loop.get()))   w->label = label;
            else if (auto* d = dynamic_cast<DoWhileStmt*>(loop.get())) d->label = label;
            return loop;
        }
    }
    if (match(TokenType::LBRACE)) {
        current--;
        return parseBlockStatement();
    }
    if (check(TokenType::IF)) {
        return parseIfStatement();
    }
    if (check(TokenType::FOR)) {
        return parseForStatement();
    }
    if (check(TokenType::WHILE)) {
        return parseWhileStatement();
    }
    if (check(TokenType::DO)) {
        return parseDoWhileStatement();
    }
    if (check(TokenType::SWITCH)) {
        return parseSwitchStatement();
    }
    if (check(TokenType::MATCH)) {
        return parseMatchStatement();
    }
    if (match(TokenType::RETURN)) {
        return parseReturnStatement();
    }
    if (match(TokenType::BREAK)) {
        return parseBreakStatement();
    }
    if (match(TokenType::CONTINUE)) {
        return parseContinueStatement();
    }
    // throw expr;
    if (match(TokenType::THROW)) {
        Token thTok = tokens[current - 1];
        ExprPtr val = parseExpression();
        consume(TokenType::SEMICOLON, "Expected ';' after throw");
        auto s = std::make_shared<ThrowStmt>(val);
        s->line = thTok.line; s->col = thTok.column;
        return s;
    }

    // defer stmt;    (runs stmt at block exit on every path out)
    // errdefer stmt; (runs only on the error path: `?`-propagation)
    if (check(TokenType::DEFER) || check(TokenType::ERRDEFER)) {
        bool isErr = check(TokenType::ERRDEFER);
        Token dTok = advance();
        StmtPtr body = parseStatement();
        auto s = std::make_shared<DeferStmt>(body, isErr);
        s->line = dTok.line; s->col = dTok.column;
        return s;
    }

    // try { } catch (Type name) { } finally { }
    if (check(TokenType::TRY)) {
        Token tryTok = advance();
        StmtPtr body = parseBlockStatement();

        std::vector<TryStmt::CatchClause> catches;
        while (check(TokenType::CATCH)) {
            advance(); // consume 'catch'
            consume(TokenType::LPAREN, "Expected '(' after catch");
            std::string ctype = parseType();
            Token nameTok = consume(TokenType::IDENT, "Expected variable name in catch");
            consume(TokenType::RPAREN, "Expected ')'");
            StmtPtr cbody = parseBlockStatement();
            catches.push_back({ctype, nameTok.value, cbody, nameTok.line, nameTok.column});
        }

        StmtPtr fin = nullptr;
        if (check(TokenType::FINALLY)) {
            advance();
            fin = parseBlockStatement();
        }

        auto s = std::make_shared<TryStmt>(body, catches, fin);
        s->line = tryTok.line; s->col = tryTok.column;
        return s;
    }

    // thread_join(tid);
    if (check(TokenType::THREAD_JOIN)) {
        Token jTok = advance();
        consume(TokenType::LPAREN, "Expected '(' after thread_join");
        ExprPtr tid = parseExpression();
        consume(TokenType::RPAREN, "Expected ')'");
        consume(TokenType::SEMICOLON, "Expected ';'");
        auto stmt = std::make_shared<ThreadJoinStmt>(tid);
        stmt->line = jTok.line; stmt->col = jTok.column;
        return stmt;
    }

    // asm("string") or asm("string" : "=r"(out), ... : "constraint"(expr), ... : "clobber", ...)
    if (check(TokenType::ASM)) {
        Token asmTok = advance();
        consume(TokenType::LPAREN, "Expected '(' after asm");
        std::string asmStr = consume(TokenType::STRING_LIT, "Expected asm string").value;

        std::vector<std::pair<std::string, ExprPtr>> outputs;
        std::vector<std::pair<std::string, ExprPtr>> inputs;
        std::vector<std::string> clobbers;
        auto operands = [&](std::vector<std::pair<std::string, ExprPtr>>& list) {
            while (!check(TokenType::RPAREN) && !check(TokenType::COLON) && !is_at_end()) {
                std::string constraint = consume(TokenType::STRING_LIT,
                    "Expected constraint string").value;
                consume(TokenType::LPAREN, "Expected '(' after constraint");
                ExprPtr expr = parseExpression();
                consume(TokenType::RPAREN, "Expected ')'");
                list.push_back({constraint, expr});
                if (!match(TokenType::COMMA)) break;
            }
        };

        if (match(TokenType::COLON)) {        // outputs
            operands(outputs);
            if (match(TokenType::COLON)) {    // inputs
                operands(inputs);
                if (match(TokenType::COLON)) { // clobbers
                    while (!check(TokenType::RPAREN) && !is_at_end()) {
                        clobbers.push_back(consume(TokenType::STRING_LIT,
                            "Expected clobber string").value);
                        if (!match(TokenType::COMMA)) break;
                    }
                }
            }
        }

        consume(TokenType::RPAREN, "Expected ')'");
        consume(TokenType::SEMICOLON, "Expected ';' after asm");
        auto stmt = std::make_shared<AsmStmt>(asmStr, outputs, inputs, clobbers);
        stmt->line = asmTok.line; stmt->col = asmTok.column;
        return stmt;
    }
    return parseExpressionStatement(true);
}

// A block item: a local declaration, or else a statement.
BlockItem Parser::parseBlockItem() {
    // `a * b;` with `a` a variable in scope is an expression (C: a variable name is not
    // a type there), not a declaration of `b` as a pointer to `a`.
    bool localTimes = check(TokenType::IDENT) && peek_ahead(1).type == TokenType::STAR &&
                      isLocalVar(peek().value);
    // Check if this looks like a declaration
    if (!localTimes && (check(TokenType::CONST) ||
        check(TokenType::VOLATILE) ||
        check(TokenType::STATIC) ||
        check(TokenType::QUESTION) ||   // `?*T q = ...` nullable-pointer local
        check(TokenType::FN) ||         // `fn(int)->int f = ...` (fn only names a type)
        check(TokenType::LET) ||
        check(TokenType::STAR) || check(TokenType::IDENT) ||
        isPrimitiveTypeToken(peek().type))) {

        DeclPtr decl = speculateIf([&]() -> DeclPtr {
            DeclPtr d = parseDeclaration();
            if (d) {
                if (auto* vd = dynamic_cast<VarDecl*>(d.get())) localVars.push_back(vd->name);
                return d;
            }
            throw ParseError("not a declaration");
        }, [](TokenType startTok) {
            // Only an identifier or a leading '*' is ambiguous (it can also
            // begin an expression statement); fall back for those. A leading
            // type keyword / const / volatile / let is unambiguously a
            // declaration, so its error is real: surface it instead of
            // masking it with a misleading expression-parse error (keeps the
            // "expected a name, found keyword 'fn'" diagnostic for `int fn;`).
            return startTok == TokenType::IDENT || startTok == TokenType::STAR;
        });
        if (decl) return decl;
    }
    return parseStatement();
}

StmtPtr Parser::parseBlockStatement() {
    Token lbTok = consume(TokenType::LBRACE, "Expected '{'");
    std::vector<BlockItem> items;
    LocalScope scope(*this);

    while (!check(TokenType::RBRACE) && !is_at_end()) {
        // A #pragma in a body updates parser state (pack / link) and emits nothing.
        if (check(TokenType::PRAGMA)) { Token pt = advance(); applyPragma(pt); continue; }
        items.push_back(parseBlockItem());
    }

    consume(TokenType::RBRACE, "Expected '}'");
    return withPos(std::make_shared<BlockStmt>(items), lbTok);
}

// An `else if` chain is parsed with a loop: each `else if` becomes the else branch of
// the one before. Every link still counts as a nesting level (the chain is nested in
// the AST, and passes that do not flatten it recurse once per link).
StmtPtr Parser::parseIfStatement() {
    struct RestoreNesting {
        Parser& p;
        int saved;
        ~RestoreNesting() { p.nesting = saved; }
    } restore{*this, nesting};
    std::shared_ptr<IfStmt> head, tail;
    for (;;) {
        Token ifTok = consume(TokenType::IF, "Expected 'if'");
        consume(TokenType::LPAREN, "Expected '('");
        ExprPtr condition = parseExpression();
        consume(TokenType::RPAREN, "Expected ')'");
        StmtPtr thenBranch = parseStatement();
        auto node = withPos(std::make_shared<IfStmt>(condition, thenBranch, nullptr), ifTok);
        if (tail) tail->elseBranch = node; else head = node;
        tail = node;
        if (!match(TokenType::ELSE)) break;
        if (!check(TokenType::IF)) { tail->elseBranch = parseStatement(); break; }
        enterNesting();
    }
    return head;
}

StmtPtr Parser::parseForStatement() {
    Token forTok = consume(TokenType::FOR, "Expected 'for'");
    consume(TokenType::LPAREN, "Expected '('");

    // for (x in iterable) — element-wise iteration
    if (check(TokenType::IDENT) && peek_ahead(1).type == TokenType::IN) {
        Token nameTok = advance();                // x
        consume(TokenType::IN, "Expected 'in'");
        ExprPtr first = parseExpression();
        // for (i in A..B) — half-open numeric range [A, B). Desugar at parse time
        // into a counted `for (T i = A, __end_i = B; i < __end_i; i = i + 1)`, so it
        // reuses all the for-loop machinery (codegen, the async transform,
        // break/continue). The bound B is evaluated ONCE, before the first iteration.
        // The bound's name is fresh against every name in the loop, so a user
        // `__end_i` read in the body (or in B) still means the user's variable.
        // T is the bounds' common integer type; the decls are spelled `int` here
        // and the type checker retypes them (VarDecl::rangeBound). Both bounds are read
        // in the enclosing scope: a B that names the loop variable (`for (i in 0..i)`)
        // means the OUTER `i`, so then the bound is declared first, before the new `i`
        // (A already sees the outer one: a local is bound after its initializer).
        if (match(TokenType::RANGE)) {
            ExprPtr end = parseExpression();
            consume(TokenType::RPAREN, "Expected ')'");
            StmtPtr body = parseStatement();
            auto iv = [&]() { return withPos(std::make_shared<IdentExpr>(nameTok.value), nameTok); };
            auto idecl = std::make_shared<VarDecl>(nameTok.value, "int", first);
            idecl->line = nameTok.line; idecl->col = nameTok.column;
            idecl->rangeBound = true;
            std::set<std::string> used{nameTok.value};
            astwalk::collectNames(first.get(), used);
            astwalk::collectNames(end.get(), used);
            astwalk::collectNames(body.get(), used);
            std::string endName = astwalk::freshName("__end_" + nameTok.value, used);
            auto edecl = std::make_shared<VarDecl>(endName, "int", end);
            edecl->line = nameTok.line; edecl->col = nameTok.column;
            edecl->rangeBound = true;
            StmtPtr init = astwalk::referencesName(end.get(), nameTok.value)
                ? std::make_shared<BlockStmt>(std::vector<BlockItem>{ DeclPtr(edecl), DeclPtr(idecl) })
                : std::make_shared<BlockStmt>(std::vector<BlockItem>{ DeclPtr(idecl), DeclPtr(edecl) });
            ExprPtr cond = std::make_shared<BinaryExpr>(iv(), "<",
                               withPos(std::make_shared<IdentExpr>(endName), nameTok));
            ExprPtr one  = std::make_shared<LiteralExpr>(LiteralExpr::Kind::INT, "1");
            ExprPtr step = std::make_shared<BinaryExpr>(iv(), "=",
                               std::make_shared<BinaryExpr>(iv(), "+", one));
            auto fs = std::make_shared<ForStmt>(init, cond, step, body);
            fs->line = nameTok.line; fs->col = nameTok.column;
            return fs;
        }
        consume(TokenType::RPAREN, "Expected ')'");
        StmtPtr body = parseStatement();
        auto fin = std::make_shared<ForInStmt>(nameTok.value, first, body);
        fin->line = nameTok.line; fin->col = nameTok.column;
        return fin;
    }

    StmtPtr init = nullptr;
    if (!check(TokenType::SEMICOLON)) {
        // Try declaration (e.g. int i = 0;) then fall back to expression
        init = speculateIf([&]() -> StmtPtr {
            DeclPtr decl = parseDeclaration();
            if (decl) {
                if (auto* vd = dynamic_cast<VarDecl*>(decl.get())) localVars.push_back(vd->name);
                return std::make_shared<BlockStmt>(std::vector<BlockItem>{decl});
            }
            throw ParseError("not a declaration");
        }, [](TokenType startTok) {
            // Unambiguous decl starts (type keyword/const/volatile/let) surface
            // their real error; only IDENT/'*' fall back to an expression.
            return startTok == TokenType::IDENT || startTok == TokenType::STAR;
        });
        if (!init) {
            init = parseExpressionStatement();
        }
    } else {
        advance();
    }

    ExprPtr condition = nullptr;
    if (!check(TokenType::SEMICOLON)) {
        condition = parseExpression();
    }
    consume(TokenType::SEMICOLON, "Expected ';'");

    ExprPtr step = nullptr;
    if (!check(TokenType::RPAREN)) {
        step = parseExpression();
    }
    consume(TokenType::RPAREN, "Expected ')'");

    StmtPtr body = parseStatement();

    return withPos(std::make_shared<ForStmt>(init, condition, step, body), forTok);
}

StmtPtr Parser::parseWhileStatement() {
    Token wTok = consume(TokenType::WHILE, "Expected 'while'");
    consume(TokenType::LPAREN, "Expected '('");
    ExprPtr condition = parseExpression();
    consume(TokenType::RPAREN, "Expected ')'");

    StmtPtr body = parseStatement();

    return withPos(std::make_shared<WhileStmt>(condition, body), wTok);
}

StmtPtr Parser::parseDoWhileStatement() {
    Token dTok = consume(TokenType::DO, "Expected 'do'");
    StmtPtr body = parseStatement();
    consume(TokenType::WHILE, "Expected 'while' after do-body");
    consume(TokenType::LPAREN, "Expected '('");
    ExprPtr condition = parseExpression();
    consume(TokenType::RPAREN, "Expected ')'");
    consume(TokenType::SEMICOLON, "Expected ';' after do-while");
    return withPos(std::make_shared<DoWhileStmt>(body, condition), dTok);
}

StmtPtr Parser::parseReturnStatement() {
    Token retTok = tokens[current - 1]; // the 'return' token
    ExprPtr value = nullptr;
    if (!check(TokenType::SEMICOLON)) {
        value = parseExpression();
    }
    rejectTrailingBlock();
    consume(TokenType::SEMICOLON, "Expected ';'");
    auto stmt = std::make_shared<ReturnStmt>(value);
    stmt->line = retTok.line; stmt->col = retTok.column;
    return stmt;
}

StmtPtr Parser::parseBreakStatement() {
    Token tok = tokens[current - 1]; // the 'break' token
    std::string label;
    if (check(TokenType::IDENT)) label = advance().value;   // break label;
    consume(TokenType::SEMICOLON, "Expected ';'");
    auto stmt = std::make_shared<BreakStmt>();
    stmt->label = label;
    stmt->line = tok.line; stmt->col = tok.column;
    return stmt;
}

StmtPtr Parser::parseContinueStatement() {
    Token tok = tokens[current - 1]; // the 'continue' token
    std::string label;
    if (check(TokenType::IDENT)) label = advance().value;   // continue label;
    consume(TokenType::SEMICOLON, "Expected ';'");
    auto stmt = std::make_shared<ContinueStmt>();
    stmt->label = label;
    stmt->line = tok.line; stmt->col = tok.column;
    return stmt;
}

StmtPtr Parser::parseExpressionStatement(bool trailing) {
    Token startTok = peek();
    size_t savedStart = trailingStart;
    trailingStart = trailing ? current : static_cast<size_t>(-1);
    ExprPtr expr;
    try {
        expr = parseExpression();
    } catch (...) {
        trailingStart = savedStart;
        throw;
    }
    trailingStart = savedStart;
    if (check(TokenType::LBRACE)) {
        if (!trailing) rejectTrailingBlock();
        Token brace = peek();
        // f(a) { BODY } is f(a, void() { BODY }): the block follows the call's own ')'.
        auto* call = dynamic_cast<CallExpr*>(expr.get());
        auto* tcall = dynamic_cast<TemplateCallExpr*>(expr.get());
        bool closed = lastCallClose != static_cast<size_t>(-1) && lastCallClose + 1 == current;
        if (!call && !tcall) fail("a trailing block must follow a call", brace);
        if (!closed) fail("a trailing block must follow the call's own ')', not parentheses around it", brace);
        if (call && dynamic_cast<CallExpr*>(call->callee.get()))
            fail("a trailing block cannot follow a call of what a call returns", brace);
        StmtPtr body;
        {
            LocalScope scope(*this);
            body = parseBlockStatement();
        }
        auto block = std::make_shared<LambdaExpr>(std::vector<std::pair<std::string, std::string>>{}, "void", body);
        block->trailing = true;
        block->line = brace.line; block->col = brace.column;
        if (call) call->args.push_back(block); else tcall->args.push_back(block);
        // One ';' after the block is accepted (lambdas are written `};`).
        match(TokenType::SEMICOLON);
        return withPos(std::make_shared<ExprStmt>(expr), startTok);
    }
    consume(TokenType::SEMICOLON, "Expected ';'");
    return withPos(std::make_shared<ExprStmt>(expr), startTok);
}

void Parser::rejectTrailingBlock() {
    if (check(TokenType::LBRACE) && lastCallClose != static_cast<size_t>(-1) && lastCallClose + 1 == current)
        fail("a trailing block is only allowed on a call statement");
}

// ============================================================================
// Expressions (Precedence Climbing)
// ============================================================================

// match subject { Variant(b0, b1) -> stmt   Other -> stmt   _ -> stmt }
StmtPtr Parser::parseMatchStatement() {
    Token kw = consume(TokenType::MATCH, "Expected 'match'");
    // Parse the subject without treating a trailing `Name { ... }` as a struct
    // literal — the `{` opens the match body (cf. Rust's match/if rule). Wrap the
    // subject in parens if a struct literal is genuinely needed there.
    // RAII so the flag is restored even if parseExpression throws — otherwise a
    // parse error in the subject would leave struct literals disabled for the
    // rest of the file. Restores at the end of this block (before the arm bodies,
    // which legitimately contain struct literals).
    ExprPtr subject;
    {
        struct NslGuard { bool& f; bool saved; NslGuard(bool& x) : f(x), saved(x) { f = true; } ~NslGuard() { f = saved; } } guard(noStructLiteral);
        subject = parseExpression();
    }
    consume(TokenType::LBRACE, "Expected '{' after match subject");
    std::vector<MatchStmt::Arm> arms;
    while (!check(TokenType::RBRACE) && !is_at_end()) {
        MatchStmt::Arm arm;
        if (check(TokenType::IDENT) && peek().value == "_") {
            advance();                              // `_` default arm
        } else {
            arm.variant = consume(TokenType::IDENT, "Expected variant name or '_'").value;
            if (match(TokenType::LPAREN)) {         // payload bindings
                if (!check(TokenType::RPAREN)) {
                    do {
                        arm.bindings.push_back(consume(TokenType::IDENT, "Expected binding name").value);
                    } while (match(TokenType::COMMA));
                }
                consume(TokenType::RPAREN, "Expected ')' after match bindings");
            }
        }
        consume(TokenType::ARROW, "Expected '->' after match pattern");
        arm.body = parseStatement();
        arms.push_back(std::move(arm));
    }
    consume(TokenType::RBRACE, "Expected '}' to close match");
    auto ms = std::make_shared<MatchStmt>(subject, std::move(arms));
    ms->line = kw.line; ms->col = kw.column;
    return ms;
}

StmtPtr Parser::parseSwitchStatement() {
    Token swTok = consume(TokenType::SWITCH, "Expected 'switch'");
    consume(TokenType::LPAREN, "Expected '('");
    ExprPtr subject = parseExpression();
    consume(TokenType::RPAREN, "Expected ')'");
    consume(TokenType::LBRACE, "Expected '{'");

    std::vector<SwitchStmt::Case> cases;
    LocalScope scope(*this);           // a case's declaration reaches the end of the switch
    while (!check(TokenType::RBRACE) && !is_at_end()) {
        SwitchStmt::Case c;
        if (match(TokenType::CASE)) {
            c.value = parseExpression();
            consume(TokenType::COLON, "Expected ':' after case value");
        } else if (match(TokenType::DEFAULT)) {
            consume(TokenType::COLON, "Expected ':' after default");
            c.value = nullptr;
        } else {
            break;
        }
        // Collect statements until the next case/default/}
        while (!check(TokenType::CASE) && !check(TokenType::DEFAULT) &&
               !check(TokenType::RBRACE) && !is_at_end()) {
            c.stmts.push_back(parseBlockItem());
        }
        cases.push_back(std::move(c));
    }
    consume(TokenType::RBRACE, "Expected '}'");
    return withPos(std::make_shared<SwitchStmt>(subject, std::move(cases)), swTok);
}
