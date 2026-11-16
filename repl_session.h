// repl_session.h: the Repl class, the JIT and building an input (internal to repl.cpp, which includes it once, in order).
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>
#include "repl.h"
#include "codegen/codegen.h"
#include "sema/type_checker.h"

namespace {

class Repl {
public:
    explicit Repl(bool interactive) : interactive(interactive) {}
    int run(const std::vector<std::string>& files);

private:
    bool interactive;
    std::vector<Chunk> chunks;
    std::vector<ReplGlobal> globals;
    int epoch = 0;
    std::unique_ptr<llvm::orc::LLJIT> jit;
    // Per symbol the JIT defines: its current version and the input that defined it.
    std::map<std::string, int> version, defEpoch;
    // The input that last declared a name, and a prefix (`Point_` for a struct's methods,
    // `id_` for a generic function's instances, "" for every function after a type is
    // redefined): a symbol defined before that is stale and gets a new version.
    std::map<std::string, int> declEpoch, prefixEpoch;
    std::set<std::string> loadedLibs;
    bool asyncSupport = false;   // kAsyncSupport is part of the session

    bool initJit();
    void reset();
    bool lexInto(ParseCtx& ctx, const std::string& text, const std::string& file,
                 std::vector<Token>& toks);
    std::shared_ptr<Program> parseTokens(ParseCtx& ctx, std::vector<Token> toks,
                                         const std::string& file);
    std::vector<size_t> untypedLets(const std::vector<Token>& toks) const;
    bool build(const Plan& plan, Attempt& a);
    bool typeOfLast(const Attempt& a, const TypeChecker& tc, std::string& semaType,
                    bool& printable) const;
    bool handleInput(const std::string& text, const std::string& file, Mode* forced);
    bool inferLets(Plan& plan, const std::vector<std::pair<int, int>>& letPos);
    bool compileAndRun(Attempt& a, const std::string& valueSemaType,
                       const std::string& valueDisplay);
    void processModule(llvm::Module& m, std::map<std::string, int>& ver,
                       std::map<std::string, int>& defEp);
    bool isStale(const std::string& sym, bool isFunction) const;
    void commit(const Plan& plan, Attempt& a);
    void typeCommand(const std::string& expr);
    void loadCommand(const std::string& path);
    void loadLibs(const std::vector<std::string>& libs);
};

bool Repl::initJit() {
    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) {
        llvm::errs() << "error: " << llvm::toString(jtmb.takeError()) << "\n";
        return false;
    }
    auto j = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*jtmb)).create();
    if (!j) {
        llvm::errs() << "error: " << llvm::toString(j.takeError()) << "\n";
        return false;
    }
    jit = std::move(*j);
    auto& jd = jit->getMainJITDylib();
    auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        jit->getDataLayout().getGlobalPrefix());
    if (!gen) {
        llvm::errs() << "error: " << llvm::toString(gen.takeError()) << "\n";
        return false;
    }
    jd.addGenerator(std::move(*gen));
    // The C++ exception runtime the compiled code throws through: taken from this binary,
    // where it is linked in (statically or not), so a lookup never depends on how the
    // compiler itself was linked.
    llvm::orc::SymbolMap eh;
    auto add = [&](const char* name, const void* addr) {
        eh[jit->mangleAndIntern(name)] = {llvm::orc::ExecutorAddr::fromPtr(addr),
                                          llvm::JITSymbolFlags::Exported};
    };
    add("__cxa_allocate_exception", (const void*)&__cxxabiv1::__cxa_allocate_exception);
    add("__cxa_free_exception", (const void*)&__cxxabiv1::__cxa_free_exception);
    add("__cxa_throw", (const void*)&__cxxabiv1::__cxa_throw);
    add("__cxa_begin_catch", (const void*)&__cxxabiv1::__cxa_begin_catch);
    add("__cxa_end_catch", (const void*)&__cxxabiv1::__cxa_end_catch);
    add("__cxa_rethrow", (const void*)&__cxxabiv1::__cxa_rethrow);
#if !defined(_WIN32)
    add("__gxx_personality_v0", (const void*)&__gxx_personality_v0);
#else
    // A Windows executable exports nothing, so what the stdlib reaches beyond the C
    // runtime DLLs comes from this binary too.
    add("__gxx_personality_seh0", (const void*)&__gxx_personality_seh0);
    add("__mingw_vsnprintf", (const void*)&__mingw_vsnprintf);
#endif
    add("_ZTIPv", (const void*)&typeid(void*));
    if (auto err = jd.define(llvm::orc::absoluteSymbols(std::move(eh)))) {
        llvm::errs() << "error: " << llvm::toString(std::move(err)) << "\n";
        return false;
    }
    return true;
}

void Repl::reset() {
    chunks.clear();
    globals.clear();
    asyncSupport = false;
    version.clear();
    defEpoch.clear();
    declEpoch.clear();
    prefixEpoch.clear();
    jit.reset();
    initJit();
}

void Repl::loadLibs(const std::vector<std::string>& libs) {
    for (const auto& l : libs) {
        if (!loadedLibs.insert(l).second) continue;
        for (const std::string& f : {"lib" + l + ".so", "lib" + l + ".so.6", "lib" + l + ".dylib",
                                     l + ".dll"}) {
            std::string err;
            if (!llvm::sys::DynamicLibrary::LoadLibraryPermanently(f.c_str(), &err)) break;
        }
    }
}

bool Repl::lexInto(ParseCtx& ctx, const std::string& text, const std::string& file,
                   std::vector<Token>& toks) {
    PPImportHook hook = ctx.cache.hookFor(isReplFile(file) ? "." : dirOf(file));
    Lexer lexer(text, &ctx.macros, file, &hook);
    for (Token t = lexer.next_token(); t.type != TokenType::EOF_TOKEN; t = lexer.next_token())
        toks.push_back(t);
    return !lexer.hadError;
}

std::shared_ptr<Program> Repl::parseTokens(ParseCtx& ctx, std::vector<Token> toks,
                                           const std::string& file) {
    int line = toks.empty() ? 1 : toks.back().line;
    toks.push_back(Token(TokenType::EOF_TOKEN, "", line, 0));
    Parser parser(toks);
    parser.filename = file;
    parser.stdlibPath = stdlibRoot;
    parser.basedir = isReplFile(file) ? "." : dirOf(file);
    parser.macros = &ctx.macros;
    parser.importedFiles = &ctx.importedFiles;
    parser.importCache = &ctx.cache;
    parser.sharedTypeNames = &ctx.typeNames;
    parser.sharedGenericNames = &ctx.genericNames;
    auto prog = parser.parse();
    if (prog) ctx.linkLibs.insert(ctx.linkLibs.end(), prog->linkLibs.begin(), prog->linkLibs.end());
    return prog;
}

// Top-level `let NAME = E;` statements (no type written): the REPL infers their type.
std::vector<size_t> Repl::untypedLets(const std::vector<Token>& toks) const {
    std::vector<size_t> out;
    int depth = 0;
    for (size_t i = 0; i < toks.size(); ++i) {
        TokenType t = toks[i].type;
        if (t == TokenType::LPAREN || t == TokenType::LBRACE || t == TokenType::LBRACKET) ++depth;
        else if (t == TokenType::RPAREN || t == TokenType::RBRACE || t == TokenType::RBRACKET) --depth;
        else if (t == TokenType::LET && depth == 0 && i + 2 < toks.size() &&
                 toks[i + 1].type == TokenType::IDENT && toks[i + 2].type == TokenType::EQ &&
                 (i == 0 || toks[i - 1].type == TokenType::SEMICOLON ||
                  toks[i - 1].type == TokenType::RBRACE))
            out.push_back(i);
    }
    return out;
}

bool Repl::build(const Plan& plan, Attempt& a) {
    a.ctx = std::make_unique<ParseCtx>();
    ParseCtx& ctx = *a.ctx;
    struct Item { int epoch; DeclPtr decl; };
    std::vector<Item> items;
    if (asyncSupport || needsAsyncSupport(plan.text)) {
        const std::string file = "<repl-async>";
        std::vector<Token> toks;
        if (!lexInto(ctx, kAsyncSupport, file, toks)) return false;
        auto prog = parseTokens(ctx, toks, file);
        if (!prog) return false;
        for (const auto& d : prog->declarations) {
            d->fromImport = true;
            items.push_back({0, d});
        }
        a.asyncSupport = true;
    }
    for (const auto& c : chunks) {
        // An earlier prompt input is named by its number in diagnostics.
        std::string file = c.file == kReplFile ? "<repl:" + std::to_string(c.epoch) + ">" : c.file;
        std::vector<Token> toks;
        if (!lexInto(ctx, c.text, file, toks)) return false;
        auto prog = parseTokens(ctx, toks, file);
        if (!prog) return false;
        for (const auto& d : prog->declarations) items.push_back({c.epoch, d});
    }
    std::vector<Item> globalItems;
    for (const auto& g : globals) {
        auto v = std::make_shared<VarDecl>(g.name, g.type);
        v->sourceFile = "<repl:" + std::to_string(g.epoch) + ">";
        v->line = g.line;
        v->col = g.col;
        globalItems.push_back({g.epoch, v});
    }

    std::vector<Token> toks;
    if (!lexInto(ctx, plan.text, plan.file, toks)) return false;
    int cur = epoch + 1;
    if (plan.mode == Mode::Decls) {
        auto prog = parseTokens(ctx, toks, plan.file);
        if (!prog) return false;
        a.inputDecls = prog->declarations;
        for (const auto& d : prog->declarations) items.push_back({cur, d});
    } else {
        for (const auto& t : toks)
            if (t.type == TokenType::AWAIT) a.usesAwait = true;
        // The input's statements, with the untyped lets typed (or cut at the probed one).
        std::map<size_t, size_t> letOrd;   // LET token index -> ordinal
        for (size_t li : untypedLets(toks)) {
            size_t n = letOrd.size();
            letOrd[li] = n;
            a.letPos.push_back({toks[li + 1].line, toks[li + 1].column});
        }
        std::vector<Token> body;
        for (size_t i = 0; i < toks.size(); ++i) {
            auto ord = letOrd.find(i);
            if (plan.probeLet >= 0 && ord != letOrd.end() && (long)ord->second == plan.probeLet) {
                // `(E);`: parenthesized, so a lambda is not read as a declaration.
                int depth = 0;
                body.push_back(Token(TokenType::LPAREN, "(", toks[i].line, toks[i].column));
                for (size_t k = i + 3; k < toks.size(); ++k) {
                    TokenType t = toks[k].type;
                    if (t == TokenType::SEMICOLON && depth == 0) break;
                    if (t == TokenType::LPAREN || t == TokenType::LBRACE || t == TokenType::LBRACKET) ++depth;
                    else if (t == TokenType::RPAREN || t == TokenType::RBRACE || t == TokenType::RBRACKET) --depth;
                    body.push_back(toks[k]);
                }
                body.push_back(Token(TokenType::RPAREN, ")", body.back().line, body.back().column));
                body.push_back(Token(TokenType::SEMICOLON, ";", body.back().line, body.back().column));
                break;
            }
            body.push_back(toks[i]);
            auto named = i >= 1 ? letOrd.find(i - 1) : letOrd.end();
            if (named != letOrd.end()) {
                auto lt = plan.letTypes.find(named->second);
                if (lt != plan.letTypes.end()) {
                    body.push_back(Token(TokenType::COLON, ":", toks[i].line, toks[i].column));
                    body.insert(body.end(), lt->second.begin(), lt->second.end());
                } else if (plan.placeholderLets) {
                    body.push_back(Token(TokenType::COLON, ":", toks[i].line, toks[i].column));
                    body.push_back(Token(TokenType::INT, "int", toks[i].line, toks[i].column));
                }
            }
        }
        if (body.empty()) return false;
        // An input that awaits runs as the body of an async function (see kAsyncSupport).
        std::string wname = (a.usesAwait ? "__repl_async_" : "__repl_") + std::to_string(cur);
        auto parseWrapped = [&](bool addSemi) {
            std::vector<Token> b = body;
            if (addSemi)
                b.push_back(Token(TokenType::SEMICOLON, ";", b.back().line,
                                  b.back().column + (int)b.back().value.size()));
            int l0 = b.front().line, c0 = b.front().column;
            std::vector<Token> wrapped;
            if (a.usesAwait) wrapped.push_back(Token(TokenType::ASYNC, "async", l0, c0));
            wrapped.insert(wrapped.end(), {
                Token(TokenType::VOID, "void", l0, c0), Token(TokenType::IDENT, wname, l0, c0),
                Token(TokenType::LPAREN, "(", l0, c0), Token(TokenType::RPAREN, ")", l0, c0),
                Token(TokenType::LBRACE, "{", l0, c0)});
            wrapped.insert(wrapped.end(), b.begin(), b.end());
            wrapped.push_back(Token(TokenType::RBRACE, "}", b.back().line, b.back().column + 1));
            return parseTokens(ctx, wrapped, plan.file);
        };
        // A final expression needs no `;`. One ending in `}` (`Point{1, 2}`, a lambda) is
        // tried with it first, then as a block statement (`if (c) { ... }`).
        std::shared_ptr<Program> prog;
        if (body.back().type == TokenType::SEMICOLON) {
            prog = parseWrapped(false);
        } else if (body.back().type == TokenType::RBRACE) {
            {
                CerrCapture cap;
                prog = parseWrapped(true);
            }
            if (!prog) prog = parseWrapped(false);
        } else {
            prog = parseWrapped(true);
        }
        if (!prog || prog->declarations.empty()) return false;
        a.wrapper = nodeCast<FunctionDecl>(prog->declarations.back().get());
        auto* blk = a.wrapper ? nodeCast<BlockStmt>(a.wrapper->body.get()) : nullptr;
        if (!blk) return false;

        // Top-level variables become globals of the session, assigned here.
        std::vector<BlockItem> kept;
        for (auto& it : blk->items) {
            auto* dp = std::get_if<DeclPtr>(&it);
            auto* vd = dp ? nodeCast<VarDecl>(dp->get()) : nullptr;
            if (!vd || vd->isExtern) { kept.push_back(it); continue; }
            a.newGlobals.push_back({vd->name, vd->type, epoch + 1, vd->line, vd->col});
            // `x = init;`. A brace list is assigned element by element for an array (the
            // global starts zeroed, so omitted elements stay zero as in C) and as a struct
            // literal otherwise.
            std::function<void(ExprPtr, const ExprPtr&, const std::string&)> assign =
                [&](ExprPtr target, const ExprPtr& init, const std::string& ty) {
                auto* al = nodeCast<ArrayLitExpr>(init.get());
                size_t open = ty.find('['), close = ty.find(']', open == std::string::npos ? 0 : open);
                if (al && !ty.empty() && ty.back() == ']' && open != std::string::npos &&
                    close != std::string::npos) {
                    std::string elemTy = ty.substr(0, open) + ty.substr(close + 1);
                    for (size_t k = 0; k < al->elements.size(); ++k) {
                        auto idx = std::make_shared<LiteralExpr>(LiteralExpr::Kind::INT,
                                                                 std::to_string(k));
                        idx->line = init->line;
                        idx->col = init->col;
                        auto el = std::make_shared<IndexExpr>(target, idx);
                        el->line = init->line;
                        el->col = init->col;
                        assign(el, al->elements[k], elemTy);
                    }
                    return;
                }
                ExprPtr value = init;
                if (al) {
                    std::vector<std::pair<std::string, ExprPtr>> inits;
                    for (const auto& e : al->elements) inits.push_back({"", e});
                    value = std::make_shared<StructInitExpr>(ty, inits);
                    value->line = init->line;
                    value->col = init->col;
                }
                auto as = std::make_shared<BinaryExpr>(target, "=", value);
                as->line = vd->line;
                as->col = vd->col;
                auto st = std::make_shared<ExprStmt>(as);
                st->line = vd->line;
                st->col = vd->col;
                kept.push_back(StmtPtr(st));
            };
            if (vd->initializer) {
                auto id = std::make_shared<IdentExpr>(vd->name);
                id->line = vd->line;
                id->col = vd->col;
                assign(id, vd->initializer, vd->type);
            }
        }
        blk->items = kept;
        for (const auto& g : a.newGlobals) {
            auto v = std::make_shared<VarDecl>(g.name, g.type);
            v->sourceFile = kReplFile;
            v->line = g.line;
            v->col = g.col;
            globalItems.push_back({cur, v});
        }
        if (!plan.valueType.empty() && !blk->items.empty()) {
            auto* sp = std::get_if<StmtPtr>(&blk->items.back());
            auto* es = sp ? nodeCast<ExprStmt>(sp->get()) : nullptr;
            if (es) {
                std::string vname = "__repl_val_" + std::to_string(cur);
                auto id = std::make_shared<IdentExpr>(vname);
                id->line = es->line;
                id->col = es->col;
                auto as = std::make_shared<BinaryExpr>(id, "=", es->expr);
                as->line = es->expr->line;
                as->col = es->expr->col;
                es->expr = as;
                auto v = std::make_shared<VarDecl>(vname, plan.valueType);
                v->sourceFile = kReplFile;
                globalItems.push_back({cur, v});
            }
        }
        items.push_back({cur, prog->declarations.back()});
        a.entry = wname;
        if (a.usesAwait) {
            a.entry = "__repl_" + std::to_string(cur);
            std::string driver = "void " + a.entry + "() { __repl_await_ok = __repl_drive((FutureHdr*)" +
                                 wname + "()); }";
            std::vector<Token> dt;
            if (!lexInto(ctx, driver, "<repl-async>", dt)) return false;
            auto dprog = parseTokens(ctx, dt, "<repl-async>");
            if (!dprog || dprog->declarations.empty()) return false;
            items.push_back({cur, dprog->declarations.back()});
        }
    }

    // A later declaration of a name replaces the earlier ones.
    std::map<std::string, int> latest;
    auto note = [&](const Item& it) {
        if (it.decl->fromImport) return;
        int& e = latest[it.decl->name];
        e = std::max(e, it.epoch);
    };
    for (const auto& it : items) note(it);
    for (const auto& it : globalItems) note(it);
    auto live = [&](const Item& it) {
        return it.decl->fromImport || latest[it.decl->name] == it.epoch;
    };
    // Types and other declarations first, then the session's variables, then functions:
    // a function may use any variable declared at the prompt.
    std::vector<DeclPtr> ordered;
    for (const auto& it : items)
        if (!nodeCast<FunctionDecl>(it.decl.get()) && live(it)) ordered.push_back(it.decl);
    for (const auto& it : globalItems)
        if (live(it)) ordered.push_back(it.decl);
    for (const auto& it : items)
        if (nodeCast<FunctionDecl>(it.decl.get()) && live(it)) ordered.push_back(it.decl);
    a.program = std::make_shared<Program>(ordered);
    a.program->linkLibs = ctx.linkLibs;
    return true;
}

}  // namespace
