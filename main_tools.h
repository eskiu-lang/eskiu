// main_tools.h: the editor modes (--hover-at, --definition-at, --references-at,
// --complete-at, --symbols); internal to main.cpp, which includes it once, in order.
#pragma once
#include "llvm/Support/CommandLine.h"
#include "sema/type_checker.h"
#include "main_support.h"

// Handle --hover-at LINE:COL
static int hoverAtMode() {
    int line = 0, col = 0;
    if (sscanf(HoverAt.c_str(), "%d:%d", &line, &col) != 2) {
        std::cerr << "error: --hover-at expects LINE:COL format\n"; return 1;
    }
    auto program = loadProgram(allInputs(), std::string(TargetTriple), Freestanding);
    if (!program) { std::cout << "(parse error)\n"; return 0; }
    try {
        TypeChecker tc; tc.targetTriple = std::string(TargetTriple); tc.warnDeprecated = false; tc.warnDefaults = false;
        tc.sourceFile = std::string(InputFilename);
        bool ok = tc.check(program.get());
        std::string type = tc.getTypeAtPosition(line, col);
        if (type.empty()) {
            if (!ok) std::cout << "(semantic error)\n";
            else     std::cout << "(no type at " << line << ":" << col << ")\n";
        } else {
            std::cout << type << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "error: internal error during hover: " << e.what() << "\n";
        std::cout << "(internal error)\n";
        return 1;
    } catch (...) {
        std::cerr << "error: unknown internal error during hover\n";
        std::cout << "(internal error)\n";
        return 1;
    }
    return 0;
}

// Handle --definition-at LINE:COL
static int definitionAtMode() {
    int line = 0, col = 0;
    if (sscanf(DefinitionAt.c_str(), "%d:%d", &line, &col) != 2) {
        std::cerr << "error: --definition-at expects LINE:COL format\n"; return 1;
    }
    auto program = loadProgram(allInputs(), std::string(TargetTriple), Freestanding);
    if (!program) { std::cout << "(parse error)\n"; return 0; }
    try {
        TypeChecker tc; tc.targetTriple = std::string(TargetTriple); tc.warnDeprecated = false; tc.warnDefaults = false;
        tc.sourceFile = std::string(InputFilename);
        bool ok = tc.check(program.get());
        std::string loc = tc.getDefinitionAt(line, col);
        if (loc.empty()) {
            if (!ok) std::cout << "(semantic error)\n";
            else     std::cout << "(no definition at " << line << ":" << col << ")\n";
        } else {
            std::cout << loc << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "error: internal error during definition: " << e.what() << "\n";
        std::cout << "(internal error)\n";
        return 1;
    } catch (...) {
        std::cerr << "error: unknown internal error during definition\n";
        std::cout << "(internal error)\n";
        return 1;
    }
    return 0;
}

// Handle --references-at LINE:COL
static int referencesAtMode() {
    int line = 0, col = 0;
    if (sscanf(ReferencesAt.c_str(), "%d:%d", &line, &col) != 2) {
        std::cerr << "error: --references-at expects LINE:COL format\n"; return 1;
    }
    auto program = loadProgram(allInputs(), std::string(TargetTriple), Freestanding);
    if (!program) { std::cout << "(parse error)\n"; return 0; }
    try {
        TypeChecker tc; tc.targetTriple = std::string(TargetTriple); tc.warnDeprecated = false; tc.warnDefaults = false;
        tc.sourceFile = std::string(InputFilename);
        bool ok = tc.check(program.get());
        auto refs = tc.getReferencesAt(line, col);
        if (refs.empty()) std::cout << (ok ? "(no symbol at " + std::to_string(line) + ":" + std::to_string(col) + ")"
                                            : std::string("(semantic error)")) << "\n";
        for (const auto& r : refs) std::cout << r << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: internal error during references: " << e.what() << "\n";
        std::cout << "(internal error)\n";
        return 1;
    }
    return 0;
}

// Handle --complete-at LINE:COL: the identifier being typed at the cursor (or the
// member after `recv.`) is replaced by TypeChecker::kCompleteSentinel, which
// collects the candidates when the type checker reaches it.
static int completeAtMode() {
    int line = 0, col = 0;
    if (sscanf(CompleteAt.c_str(), "%d:%d", &line, &col) != 2) {
        std::cerr << "error: --complete-at expects LINE:COL format\n"; return 1;
    }
    std::string src = readFile(InputFilename);
    size_t ls = 0;
    for (int l = 1; l < line && ls != std::string::npos; ++l) {
        ls = src.find('\n', ls);
        if (ls != std::string::npos) ++ls;
    }
    if (ls == std::string::npos) ls = src.size();
    size_t le = src.find('\n', ls);
    if (le == std::string::npos) le = src.size();
    size_t at = std::min(le, ls + (size_t)std::max(0, col - 1));
    auto isId = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
    size_t p = at, q = at;
    while (p > ls && isId(src[p - 1])) --p;
    while (q < le && isId(src[q])) ++q;
    bool restBlank = src.find_first_not_of(" \t\r", q) >= le;
    std::string spliced = src.substr(0, p) + TypeChecker::kCompleteSentinel +
                          (restBlank ? ";" : "") + src.substr(q);
    // A syntax error elsewhere in the file (the user is mid-edit): blank the broken
    // line and try again. An error at the first token of a line blames the line
    // before it (an unfinished statement is reported at the next token). A line that
    // opens or closes a block is kept, and so is the cursor's line.
    std::shared_ptr<Program> program;
    std::streambuf* errBuf = std::cerr.rdbuf();
    std::ostringstream quiet;
    auto lineSpan = [&](int ln, size_t& b, size_t& e) {
        b = 0;
        for (int l = 1; l < ln && b != std::string::npos; ++l) {
            b = spliced.find('\n', b);
            if (b != std::string::npos) ++b;
        }
        if (b == std::string::npos) return false;
        e = spliced.find('\n', b);
        if (e == std::string::npos) e = spliced.size();
        return true;
    };
    for (int attempt = 0; attempt < 8 && !program; ++attempt) {
        Parser::firstErrorLine() = 0;
        std::cerr.rdbuf(quiet.rdbuf());
        program = loadProgram(allInputs(), std::string(TargetTriple), Freestanding,
                              nullptr, nullptr, &spliced);
        std::cerr.rdbuf(errBuf);
        if (program) break;
        int bad = Parser::firstErrorLine();
        if (bad <= 0 || Parser::canonicalPath(Parser::firstErrorFile()) !=
                            Parser::canonicalPath(std::string(InputFilename))) break;
        size_t b = 0, e = 0;
        if (!lineSpan(bad, b, e)) break;
        size_t first = spliced.find_first_not_of(" \t", b);
        if (first == std::string::npos || first >= e || (int)(first - b) + 1 >= Parser::firstErrorCol()) {
            do {
                --bad;
            } while (bad > 0 && lineSpan(bad, b, e) &&
                     spliced.find_first_not_of(" \t\r", b) >= e);
        }
        if (bad <= 0 || bad == line || !lineSpan(bad, b, e)) break;
        if (spliced.substr(b, e - b).find_first_of("{}") != std::string::npos) break;
        spliced.replace(b, e - b, std::string(e - b, ' '));
    }
    if (!program) { std::cout << "(parse error)\n"; return 0; }
    try {
        TypeChecker tc; tc.targetTriple = std::string(TargetTriple); tc.warnDeprecated = false; tc.warnDefaults = false;
        tc.sourceFile = std::string(InputFilename);
        tc.check(program.get());
        for (const auto& c : tc.completions)
            std::cout << c.kind << "\t" << c.name << "\t" << c.detail << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: internal error during completion: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

// Handle --symbols: the declarations of the input file (an editor's outline).
static int symbolsMode() {
    auto program = loadProgram(allInputs(), std::string(TargetTriple), Freestanding);
    if (!program) { std::cout << "(parse error)\n"; return 0; }
    std::string self = Parser::canonicalPath(std::string(InputFilename));
    auto mine = [&](const Decl* d) {
        return !d->fromImport && (d->sourceFile.empty() || Parser::canonicalPath(d->sourceFile) == self);
    };
    auto pos = [](int l, int c) { return std::to_string(l) + ":" + std::to_string(c); };
    auto sig = [](const FunctionDecl* f, const std::string& name) {
        std::string s = f->returnType + " " + name + "(";
        for (size_t i = 0; i < f->params.size(); ++i)
            s += (i ? ", " : "") + f->params[i].first +
                 (f->params[i].second.empty() ? "" : " " + f->params[i].second);
        return s + ")";
    };
    auto emit = [](const std::string& kind, const std::string& name, const std::string& at,
                   const std::string& container, const std::string& detail) {
        std::cout << kind << "\t" << name << "\t" << at << "\t" << container << "\t" << detail << "\n";
    };
    for (const auto& d : program->declarations) {
        if (!mine(d.get()) || d->line <= 0) continue;   // not an implicit one (a script's argc)
        if (auto* f = nodeCast<FunctionDecl>(d.get())) {
            emit("function", f->name, pos(f->line, f->col), "", sig(f, f->name));
        } else if (auto* s = nodeCast<StructDecl>(d.get())) {
            emit("struct", s->name, pos(s->line, s->col), "", "struct " + s->name);
            for (const auto& fl : s->fields)
                emit("field", fl.name, pos(fl.line, fl.col), s->name, fl.type);
            for (const auto& m : s->methods)
                if (auto* f = nodeCast<FunctionDecl>(m.get()))
                    emit("method", f->name, pos(f->line, f->col), s->name, sig(f, f->name));
        } else if (auto* u = nodeCast<UnionDecl>(d.get())) {
            emit("struct", u->name, pos(u->line, u->col), "", "union " + u->name);
            for (const auto& fl : u->fields)
                emit("field", fl.name, pos(fl.line, fl.col), u->name, fl.type);
        } else if (auto* e = nodeCast<EnumDecl>(d.get())) {
            emit("enum", e->name, pos(e->line, e->col), "", "enum " + e->name);
        } else if (auto* i = nodeCast<InterfaceDecl>(d.get())) {
            emit("interface", i->name, pos(i->line, i->col), "", "interface " + i->name);
        } else if (auto* a = nodeCast<TypeAliasDecl>(d.get())) {
            emit("type", a->name, pos(a->line, a->col), "", "type " + a->name + " = " + a->aliased);
        } else if (auto* v = nodeCast<VarDecl>(d.get())) {
            emit(v->isConst ? "constant" : "variable", v->name, pos(v->line, v->col), "", v->type);
        }
    }
    return 0;
}
