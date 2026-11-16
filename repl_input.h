// repl_input.h: exceptions, the crash guard and reading complete inputs (internal to repl.cpp, which includes it once, in order).
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>
#include "repl.h"
#include "codegen/codegen.h"
#include "sema/type_checker.h"

namespace {

// The thrown value of an Eskiu exception: { int64 size, char* type, payload }.
std::string describeException(void* ex) {
    if (!ex) return "";
    const char* p = (const char*)ex;
    const char* type = readAt<const char*>(p + 8);
    if (!type) return "";
    std::string t = type, out = t;
    const char* pay = p + 16;
    // The same primitives as a compiled program's uncaught-exception report.
    if (t == "int" || t == "int32") out += " " + std::to_string(readAt<int32_t>(pay));
    else if (t == "int8") out += " " + std::to_string(readAt<int8_t>(pay));
    else if (t == "int16") out += " " + std::to_string(readAt<int16_t>(pay));
    else if (t == "int64") out += " " + std::to_string(readAt<long long>(pay));
    else if (t == "uint" || t == "uint32") out += " " + std::to_string(readAt<uint32_t>(pay));
    else if (t == "uint8") out += " " + std::to_string(readAt<uint8_t>(pay));
    else if (t == "uint16") out += " " + std::to_string(readAt<uint16_t>(pay));
    else if (t == "uint64") out += " " + std::to_string(readAt<unsigned long long>(pay));
    else if (t == "double" || t == "float") {
        char b[64];
        std::snprintf(b, sizeof b, " %g", t == "float" ? (double)readAt<float>(pay) : readAt<double>(pay));
        out += b;
    }
    else if (t == "bool") out += readAt<uint8_t>(pay) ? " true" : " false";
    else if (t == "char") out += std::string(" '") + readAt<char>(pay) + "'";
    else if (t == "string") {
        const char* s = readAt<const char*>(pay);
        if (s) out += " " + quoteText(s, std::strlen(s), '"');
    }
    return out;
}

// --- running an input -----------------------------------------------------------------

#if !defined(_WIN32)
sigjmp_buf gCrashJmp;
volatile sig_atomic_t gCrashSig = 0;

void onCrash(int sig) {
    gCrashSig = sig;
    siglongjmp(gCrashJmp, 1);
}
#endif

// Call the compiled input. A crash (a bad pointer, a trap) returns the signal number instead
// of ending the session; 0 when it returned normally. Exceptions pass through to the caller.
int callGuarded(void (*fn)()) {
#if defined(_WIN32)
    fn();
    return 0;
#else
    static thread_local std::vector<char> altStack;
    if (altStack.empty()) {
        altStack.resize(1 << 16);
        stack_t ss{};
        ss.ss_sp = altStack.data();
        ss.ss_size = altStack.size();
        sigaltstack(&ss, nullptr);
    }
    const int sigs[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP};
    struct sigaction sa{}, old[5];
    sa.sa_handler = onCrash;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_ONSTACK | SA_NODEFER;
    for (int i = 0; i < 5; ++i) sigaction(sigs[i], &sa, &old[i]);
    int caught = 0;
    if (sigsetjmp(gCrashJmp, 1) == 0) fn();
    else caught = gCrashSig;
    for (int i = 0; i < 5; ++i) sigaction(sigs[i], &old[i], nullptr);
    return caught;
#endif
}

// --- input scanning -------------------------------------------------------------------

// Bracket depth of `text` outside string/char literals and comments: the reader keeps
// reading lines while it is positive.
// Whether `text` is an unfinished input: it parses neither as top-level declarations
// nor as statements, and the parse fails at its very end (as Python's REPL decides).
// `1 +`, `int sq(int n)`, `do { ... }` without its `while`: wait for more lines. An
// error before the end means the input is complete and wrong: it is sent so the
// diagnostic shows.
bool incompleteInput(const std::string& text) {
    // A preprocessor conditional still open (`#ifdef X` without its `#endif`).
    {
        static const std::regex open(R"(^\s*#\s*(if|ifdef|ifndef)\b)"), close(R"(^\s*#\s*endif\b)");
        int depth = 0;
        std::istringstream in(text);
        for (std::string l; std::getline(in, l);) {
            if (std::regex_search(l, open)) ++depth;
            else if (std::regex_search(l, close)) --depth;
        }
        if (depth > 0) return true;
    }
    // A trailing `?` or `:` starts the arms of a `?:` (or labels the loop on the next
    // line); postfix `?` propagation has no function to return from at the prompt.
    std::string t = text;
    while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
    if (!t.empty() && (t.back() == '?' || (t.back() == ':' && (t.size() < 2 || t[t.size() - 2] != ':'))))
        return true;
    auto probe = [](const std::string& src, int& errLine, int& errCol, int& eofLine, int& eofCol) {
        std::map<std::string, Macro> macros;
        std::streambuf* errBuf = std::cerr.rdbuf();
        std::ostringstream lexQuiet;
        std::cerr.rdbuf(lexQuiet.rdbuf());
        struct Restore { std::streambuf* b; ~Restore() { std::cerr.rdbuf(b); } } restore{errBuf};
        Lexer lexer(src, &macros, "<probe>", nullptr);
        std::vector<Token> toks;
        Token t = lexer.next_token();
        for (; t.type != TokenType::EOF_TOKEN; t = lexer.next_token()) toks.push_back(t);
        if (lexer.hadError) return true;   // a lexical error is reported on submit
        eofLine = t.line; eofCol = t.column;
        // Imports are resolved on submit; the probe only checks the syntax around them.
        std::vector<Token> kept;
        for (size_t i = 0; i < toks.size(); ++i) {
            if (toks[i].type == TokenType::IMPORT) {
                while (i < toks.size() && toks[i].type != TokenType::SEMICOLON) ++i;
                continue;
            }
            kept.push_back(toks[i]);
        }
        kept.push_back(t);
        Parser parser(kept);
        parser.filename = "<probe>";
        Parser::firstErrorLine() = 0;
        std::streambuf* saved = std::cerr.rdbuf();
        std::ostringstream quiet;
        std::cerr.rdbuf(quiet.rdbuf());
        bool ok = false;
        try { ok = parser.parse() != nullptr; } catch (...) { ok = false; }
        std::cerr.rdbuf(saved);
        errLine = Parser::firstErrorLine();
        errCol = Parser::firstErrorCol();
        return ok;
    };
    int el = 0, ec = 0, fl = 0, fc = 0;
    if (probe(text, el, ec, fl, fc)) return false;
    bool declAtEnd = el > fl || (el == fl && ec >= fc);
    // As statements: the body of a function, as typed and with a `;` after it, so a
    // bare final expression (`x`, the usual REPL input) is complete too.
    int lines = (int)std::count(text.begin(), text.end(), '\n') + 1;
    if (probe("void __probe() {\n" + text + "\n}", el, ec, fl, fc)) return false;
    bool stmtAtEnd = el >= lines + 2;
    if (probe("void __probe() {\n" + text + "\n;}", el, ec, fl, fc)) return false;
    stmtAtEnd = stmtAtEnd && el >= lines + 2;
    return declAtEnd || stmtAtEnd;
}

// An operator a line cannot start with (`? 10`, `: 20`, `.len`, `&& b`): it continues an
// expression of the line before, which already ran. Returns it, or "".
std::string danglingOperator(const std::string& t) {
    static const char* ops[] = {"&&", "||", "==", "!=", "<=", ">=", "->", "<<", ">>",
                                "?", ".", "<", ">", "|", "^", "%", "="};
    if (t.empty()) return "";
    if (t[0] == ':') return (t.size() == 1 || !std::isalpha((unsigned char)t[1])) ? ":" : "";
    if (t[0] == '/' && t.size() > 1 && (t[1] == '/' || t[1] == '*')) return "";
    if (t[0] == '/') return "/";
    for (const char* op : ops)
        if (t.compare(0, std::strlen(op), op) == 0) return op;
    return "";
}

// The first word of `text` (`if`, `try`, ...), after blanks and comments.
std::string firstWord(const std::string& text) {
    static const std::regex lead(R"(^(?:\s|//[^\n]*\n|/\*[\s\S]*?\*/)*([A-Za-z_]\w*))");
    std::smatch m;
    return std::regex_search(text, m, lead) ? m[1].str() : "";
}

// A header whose body starts on the next line (`int sq(int n)`, `for (i in 0..3)`,
// `struct P`, `} else`), as written in the style that puts `{` on its own line: the
// reader waits for the body instead of sending an incomplete input.
bool awaitsBody(const std::string& text) {
    std::string t = text;
    while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
    size_t ls = t.find_last_of('\n');
    std::string last = t.substr(ls == std::string::npos ? 0 : ls + 1);
    size_t b = last.find_first_not_of(" \t");
    if (b == std::string::npos) return false;
    last = last.substr(b);
    static const std::regex control(R"(^(\}\s*)?(if|else\s+if|for|while|switch|match)\b.*\)$)");
    static const std::regex bare(R"(^(\}\s*)?(else|do)$)");
    static const std::regex typeHead(R"(^(packed\s+)?(struct|union|enum|interface)\s+\w+(\s*<[^>]*>)?$)");
    static const std::regex fnHead(
        R"(^(?!(await|return|throw|defer|errdefer|case|else|sizeof|free_closure|thread_join)\b)((async|must_use|static|extern|deprecated\("[^"]*"\))\s+)*[\w*<>\[\],?. ]*[\w*>\]?]\s+\*?\w+\s*(<[^>]*>)?\s*\([^;{}]*\)$)");
    // A lambda whose body comes next: `= int(int v)`, `= void()`, `= *Node(*Node a, int k)`
    // (parameters with names, or none after a type keyword), which a call never has.
    static const std::regex lambdaHead(
        R"(=\s*(([\w*<>\[\]?]+\s*\(\s*[A-Za-z_*][\w*<>\[\]?]*\s+[A-Za-z_]\w*\s*(,\s*[A-Za-z_*][\w*<>\[\]?]*\s+[A-Za-z_]\w*\s*)*\))|((\*+\w+|int\w*|uint\w*|float|double|bool|char|string|void)\s*\(\s*\)))\s*$)");
    return std::regex_match(last, control) || std::regex_match(last, bare) ||
           std::regex_match(last, typeHead) || std::regex_match(last, fnHead) ||
           std::regex_search(last, lambdaHead);
}

// A complete `if` or `try`: an `else`, `catch` or `finally` may follow on the next line,
// so it is held for one line (a blank line, or any other line, runs it).
bool ifAwaitsElse(const std::string& text) {
    std::string w = firstWord(text);
    return w == "if" || w == "try";
}

// Whether `line` continues a held `if`/`try` (`else`, `catch`, `finally`).
bool continuesHeld(const std::string& line) {
    std::string w = firstWord(line);
    if (w.empty()) {
        size_t b = line.find_first_not_of(" \t");
        return b != std::string::npos && line[b] == '}' && firstWord(line.substr(b + 1)) == "else";
    }
    return w == "else" || w == "catch" || w == "finally";
}

int bracketDepth(const std::string& text) {
    int depth = 0;
    bool block = false, str = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        char n = i + 1 < text.size() ? text[i + 1] : 0;
        if (block) {
            if (c == '*' && n == '/') { block = false; ++i; }
            continue;
        }
        if (c == '/' && n == '/') { while (i < text.size() && text[i] != '\n') ++i; continue; }
        if (c == '/' && n == '*') { block = true; ++i; continue; }
        // A string literal may span lines (a char literal may not): one still open at
        // the end keeps the input going.
        if (c == '"' || c == '\'') {
            for (++i; i < text.size() && text[i] != c && (c == '"' || text[i] != '\n'); ++i)
                if (text[i] == '\\') ++i;
            if (c == '"' && i >= text.size()) str = true;
            continue;
        }
        if (c == '(' || c == '{' || c == '[') ++depth;
        else if (c == ')' || c == '}' || c == ']') --depth;
    }
    return block || str ? std::max(depth, 1) : depth;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// The first diagnostic's position in captured compiler output, for choosing between the
// declaration and the statement reading of an input that is neither.
std::pair<int, int> firstErrorPos(const std::string& errs) {
    std::string key = std::string(kReplFile) + ":";
    size_t at = errs.find(key);
    if (at == std::string::npos) return {0, 0};
    int line = 0, col = 0;
    if (sscanf(errs.c_str() + at + key.size(), "%d:%d", &line, &col) < 1) return {0, 0};
    return {line, col};
}

}  // namespace
