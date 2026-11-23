// pp_cond.h: #if constant expressions, conditions and import scanning (internal to preprocessor.cpp, which includes it once, in order).
#pragma once
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "lexer.h"
#include "preprocessor.h"

// ── #if / #elif constant expressions ─────────────────────────────────────────
// The C subset: integer and char literals, macros (expanded first), `defined X` /
// `defined(X)`, the unary `! ~ - +`, the binary arithmetic, shift, relational,
// equality, bitwise and logical operators, `?:`, and parentheses. An identifier
// left after expansion is 0, as in C. Evaluated on 64-bit signed integers with
// two's-complement wraparound (`+ - *`, unary `-`, and INT64_MIN / -1 wrap; never
// host UB). `&&`, `||` and `?:` short-circuit: an operand that is not evaluated
// cannot raise a division-by-zero or shift-count error. An integer literal that
// does not fit in 64 bits and a shift count outside 0..63 are errors.
namespace {
struct PPExprError { std::string msg; };

struct PPExprEval {
    std::vector<std::string> toks;
    size_t pos = 0;
    int skip = 0;   // > 0 inside an operand a short-circuit leaves unevaluated

    const std::string& peek() const { static const std::string end; return pos < toks.size() ? toks[pos] : end; }
    bool eat(const char* t) { if (peek() == t) { pos++; return true; } return false; }
    void fail(const std::string& msg) { if (skip == 0) throw PPExprError{msg}; }

    static long long wrap(unsigned long long v) { return (long long)v; }

    long long primary() {
        if (pos >= toks.size()) throw PPExprError{"expected a value"};
        std::string t = toks[pos++];
        if (t == "(") {
            long long v = ternary();
            if (!eat(")")) throw PPExprError{"expected ')'"};
            return v;
        }
        if (std::isdigit((unsigned char)t[0])) {
            std::string d = t;
            while (!d.empty() && (d.back() == 'u' || d.back() == 'U' || d.back() == 'l' || d.back() == 'L')) d.pop_back();
            bool hex = d.size() > 1 && d[0] == '0' && (d[1] == 'x' || d[1] == 'X');
            bool bin = d.size() > 1 && d[0] == '0' && (d[1] == 'b' || d[1] == 'B');
            bool oct = !hex && !bin && d.size() > 1 && d[0] == '0';
            unsigned base = hex ? 16 : bin ? 2 : oct ? 8 : 10;
            size_t start = (hex || bin) ? 2 : 0;
            if (start >= d.size()) throw PPExprError{"invalid integer '" + t + "'"};
            unsigned long long v = 0;
            for (size_t i = start; i < d.size(); ++i) {
                char c = d[i];
                if (c == '_' && i > start && i + 1 < d.size()) continue;
                unsigned dv = std::isdigit((unsigned char)c) ? c - '0'
                            : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                            : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 99;
                if (dv >= base) throw PPExprError{"invalid integer '" + t + "'"};
                if (v > (~0ULL - dv) / base) throw PPExprError{"integer constant '" + t + "' is too large"};
                v = v * base + dv;
            }
            return wrap(v);
        }
        if (t[0] == '\'') {
            int v = 0; std::string err;
            if (!decodeCharLiteral(t, v, err)) throw PPExprError{err};
            return v;
        }
        if (std::isalpha((unsigned char)t[0]) || t[0] == '_') return 0;
        throw PPExprError{"unexpected '" + t + "'"};
    }
    long long unary() {
        if (eat("!")) return !unary();
        if (eat("~")) return ~unary();
        if (eat("-")) return wrap(0ULL - (unsigned long long)unary());
        if (eat("+")) return unary();
        return primary();
    }
    long long mul() {
        long long v = unary();
        while (true) {
            if (eat("*")) v = wrap((unsigned long long)v * (unsigned long long)unary());
            else if (peek() == "/" || peek() == "%") {
                bool div = peek() == "/"; pos++;
                long long r = unary();
                if (r == 0) { fail("division by zero"); v = 0; continue; }
                if (r == -1) v = div ? wrap(0ULL - (unsigned long long)v) : 0;
                else v = div ? v / r : v % r;
            } else return v;
        }
    }
    long long add() {
        long long v = mul();
        while (true) {
            if (eat("+")) v = wrap((unsigned long long)v + (unsigned long long)mul());
            else if (eat("-")) v = wrap((unsigned long long)v - (unsigned long long)mul());
            else return v;
        }
    }
    long long shift() {
        long long v = add();
        while (true) {
            bool left = false;
            if (eat("<<")) left = true;
            else if (!eat(">>")) return v;
            long long r = add();
            if (r < 0 || r > 63) { fail("shift count " + std::to_string(r) + " is out of range"); v = 0; continue; }
            v = left ? wrap((unsigned long long)v << r) : v >> r;
        }
    }
    long long rel() {
        long long v = shift();
        while (true) {
            if (eat("<")) v = v < shift();
            else if (eat(">")) v = v > shift();
            else if (eat("<=")) v = v <= shift();
            else if (eat(">=")) v = v >= shift();
            else return v;
        }
    }
    long long eq() {
        long long v = rel();
        while (true) {
            if (eat("==")) v = v == rel();
            else if (eat("!=")) v = v != rel();
            else return v;
        }
    }
    long long band() { long long v = eq();   while (eat("&")) v = v & eq();   return v; }
    long long bxor() { long long v = band(); while (eat("^")) v = v ^ band(); return v; }
    long long bor()  { long long v = bxor(); while (eat("|")) v = v | bxor(); return v; }
    long long land() {
        long long v = bor();
        while (eat("&&")) {
            bool dead = v == 0;
            if (dead) skip++;
            long long r = bor();
            if (dead) skip--;
            v = v && r;
        }
        return v;
    }
    long long lor() {
        long long v = land();
        while (eat("||")) {
            bool dead = v != 0;
            if (dead) skip++;
            long long r = land();
            if (dead) skip--;
            v = v || r;
        }
        return v;
    }
    long long ternary() {
        long long c = lor();
        if (!eat("?")) return c;
        if (c == 0) skip++;
        long long a = ternary();
        if (c == 0) skip--;
        if (!eat(":")) throw PPExprError{"expected ':' in '?:'"};
        if (c != 0) skip++;
        long long b = ternary();
        if (c != 0) skip--;
        return c ? a : b;
    }
};
// Split an (already macro-expanded) #if expression into tokens.
std::vector<std::string> ppExprTokens(const std::string& e) {
    std::vector<std::string> out;
    size_t i = 0, n = e.size();
    while (i < n) {
        char c = e[i];
        if (c == ' ' || c == '\t') { i++; continue; }
        if (c == '/' && i + 1 < n && e[i + 1] == '/') break;
        if (c == '/' && i + 1 < n && e[i + 1] == '*') {
            size_t end = e.find("*/", i + 2);
            i = end == std::string::npos ? n : end + 2;
            continue;
        }
        if (std::isalnum((unsigned char)c) || c == '_') {
            size_t j = i; while (j < n && (std::isalnum((unsigned char)e[j]) || e[j] == '_')) j++;
            out.push_back(e.substr(i, j - i)); i = j; continue;
        }
        if (c == '\'') {
            size_t j = i + 1;
            while (j < n && e[j] != '\'') { if (e[j] == '\\') j++; j++; }
            out.push_back(e.substr(i, std::min(j + 1, n) - i)); i = j + 1; continue;
        }
        static const char* two[] = {"||", "&&", "==", "!=", "<=", ">=", "<<", ">>"};
        bool matched = false;
        for (const char* t : two) {
            if (i + 1 < n && e[i] == t[0] && e[i + 1] == t[1]) { out.push_back(t); i += 2; matched = true; break; }
        }
        if (matched) continue;
        if (std::string("()!~-+*/%<>&^|?:").find(c) != std::string::npos) { out.push_back(std::string(1, c)); i++; continue; }
        throw PPExprError{std::string("unexpected character '") + c + "'"};
    }
    return out;
}
} // namespace

// Replace `defined X` / `defined(X)` with 1/0, macro-expand, then evaluate.
// Returns false (and sets err) on a malformed expression, or one whose expansion
// spends the file's macro budget.
static bool ppEvalIf(const std::string& expr, const std::map<std::string, Macro>& defines,
                     long long& value, std::string& err, PPBudget* budget = nullptr) {
    std::string pre;
    size_t i = 0, n = expr.size();
    while (i < n) {
        char c = expr[i];
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t j = i; while (j < n && (std::isalnum((unsigned char)expr[j]) || expr[j] == '_')) j++;
            std::string id = expr.substr(i, j - i);
            if (id != "defined") { pre += id; i = j; continue; }
            size_t k = j; while (k < n && (expr[k] == ' ' || expr[k] == '\t')) k++;
            bool paren = k < n && expr[k] == '(';
            if (paren) { k++; while (k < n && (expr[k] == ' ' || expr[k] == '\t')) k++; }
            size_t m = k; while (m < n && (std::isalnum((unsigned char)expr[m]) || expr[m] == '_')) m++;
            if (m == k) { err = "expected a macro name after 'defined'"; return false; }
            std::string name = expr.substr(k, m - k);
            if (paren) {
                while (m < n && (expr[m] == ' ' || expr[m] == '\t')) m++;
                if (m >= n || expr[m] != ')') { err = "expected ')' after 'defined(" + name + "'"; return false; }
                m++;
            }
            pre += defines.count(name) ? " 1 " : " 0 ";
            i = m; continue;
        }
        if (c == '\'') {
            size_t j = i + 1;
            while (j < n && expr[j] != '\'') { if (expr[j] == '\\') j++; j++; }
            size_t e = std::min(j + 1, n);
            pre += expr.substr(i, e - i); i = e; continue;
        }
        pre += c; i++;
    }
    std::set<std::string> expanding;
    PPExpandCtx ctx;
    ctx.budget = budget;
    std::string expanded = ppExpand(pre, defines, expanding, nullptr, &ctx);
    if (!ctx.err.empty()) { err = ctx.err; return false; }
    if (budget && budget->hit) {
        if (budget->reported) { value = 0; return true; }
        budget->reported = true;
        err = ppBudgetMessage();
        return false;
    }
    try {
        PPExprEval ev;
        ev.toks = ppExprTokens(expanded);
        if (ev.toks.empty()) { err = "#if with no expression"; return false; }
        value = ev.ternary();
        if (ev.pos != ev.toks.size()) { err = "unexpected '" + ev.toks[ev.pos] + "' in #if expression"; return false; }
    } catch (const PPExprError& e) {
        err = e.msg + " in #if expression";
        return false;
    }
    return true;
}

// Does a macro body use `#` (stringification) or `##` (token pasting) outside a
// string/char literal? Neither is supported.
static bool ppBodyHasHash(const std::string& body) {
    for (size_t i = 0; i < body.size(); ++i) {
        char c = body[i];
        if (c == '"' || c == '\'') {
            char q = c; ++i;
            while (i < body.size() && body[i] != q) { if (body[i] == '\\') ++i; ++i; }
            continue;
        }
        if (c == '#') return true;
    }
    return false;
}

// A directive line with its comments replaced by whitespace (C translation phase 3
// runs before directives): each closed `/* */` becomes one space and a `//` comment
// is dropped, string and char literals are kept whole. A `/*` left open drops the
// rest of the line and sets `openAtEnd`.
static std::string ppStripComments(const std::string& line, bool& openAtEnd) {
    std::string out; size_t i = 0, n = line.size();
    openAtEnd = false;
    while (i < n) {
        char c = line[i];
        if (c == '"' || c == '\'') { ppCopyLiteral(line, i, out); continue; }
        if (c == '/' && i + 1 < n && line[i + 1] == '/') break;
        if (c == '/' && i + 1 < n && line[i + 1] == '*') {
            size_t e = line.find("*/", i + 2);
            if (e == std::string::npos) { openAtEnd = true; break; }
            out += ' '; i = e + 2; continue;
        }
        out += c; i++;
    }
    return out;
}

// Report each `import "path"` / `import <name>` on an (expanded) active line to the
// hook, in order. Comments and literals are skipped; `blk` is whether a `/* */`
// comment is open at the start of the line.
static void ppScanImports(const std::string& s, bool blk, const PPImportHook& hook) {
    size_t i = 0, n = s.size();
    while (i < n) {
        if (blk) {
            size_t e = s.find("*/", i);
            if (e == std::string::npos) return;
            i = e + 2; blk = false; continue;
        }
        char c = s[i];
        if (c == '/' && i + 1 < n && s[i + 1] == '*') { blk = true; i += 2; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') return;
        if (c == '"' || c == '\'') { std::string lit; ppCopyLiteral(s, i, lit); continue; }
        if (ppIdentStart(c)) {
            size_t j = i; while (j < n && ppIdentChar(s[j])) j++;
            if (j - i == 6 && s.compare(i, 6, "import") == 0) {
                size_t k = ppSkipBlank(s, j);
                if (k < n && s[k] == '"') {
                    size_t e = k + 1;
                    while (e < n && s[e] != '"' && s[e] != '\\') e++;
                    if (e < n && s[e] == '"') hook(s.substr(k + 1, e - k - 1), false);
                } else if (k < n && s[k] == '<') {
                    size_t e = k + 1; std::string name;
                    while (e < n && s[e] != '>' && s[e] != ';') { if (s[e] != ' ' && s[e] != '\t') name += s[e]; e++; }
                    if (e < n && s[e] == '>') hook(name, true);
                }
            }
            i = j; continue;
        }
        i++;
    }
}

// `__FILE__`'s body: the path as a string literal, with `\` and `"` escaped so a
// Windows path or a quote in a file name stays one well-formed literal.
static std::string ppFileLiteral(const std::string& path) {
    std::string s = "\"";
    for (char c : path) {
        if (c == '\\' || c == '"') { s += '\\'; s += c; }
        else if (c == '\n') s += "\\n";
        else if (c == '\t') s += "\\t";
        else if (c == '\r') s += "\\r";
        else s += c;
    }
    return s + "\"";
}

static const int kMaxMacroJoin = 1000;

// Does the next line with code (past blank and comment-only lines) begin with `(`?
// Reads ahead without consuming.
static bool ppNextCodeIsParen(std::istringstream& in) {
    std::streampos pos = in.tellg();
    bool res = false;
    std::string l;
    for (int k = 0; k < kMaxMacroJoin && std::getline(in, l); ++k) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        size_t p = ppSkipBlank(l, 0);
        if (p >= l.size() || l.compare(p, 2, "//") == 0) continue;
        res = l[p] == '(';
        break;
    }
    in.clear();
    in.seekg(pos);
    return res;
}
