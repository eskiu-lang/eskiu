// pp_expand.h: macro expansion and the line helpers (internal to preprocessor.cpp, which includes it once, in order).
#pragma once
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "lexer.h"
#include "preprocessor.h"

static std::string ppTrim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

// Copy a quoted string/char literal from s starting at s[i] (the opening quote)
// into out, advancing i past the closing quote. Backslash escapes are copied
// verbatim so a quote inside them does not end the literal.
static void ppCopyLiteral(const std::string& s, size_t& i, std::string& out) {
    size_t n = s.size();
    char q = s[i]; out += s[i]; i++;
    while (i < n) {
        if (s[i]=='\\'&&i+1<n){out+=s[i];out+=s[i+1];i+=2;continue;}
        out += s[i]; if (s[i]==q){i++;break;} i++;
    }
}

// Replace each parameter name in a function-like macro body with its argument.
static std::string ppSubstParams(const std::string& body,
                                 const std::vector<std::string>& params,
                                 const std::vector<std::string>& args) {
    std::map<std::string, std::string> m;
    for (size_t i = 0; i < params.size() && i < args.size(); ++i) m[params[i]] = args[i];
    std::string out; size_t i = 0, n = body.size();
    while (i < n) {
        char c = body[i];
        if (c == '"' || c == '\'') {
            ppCopyLiteral(body, i, out);
            continue;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t j = i; while (j<n && (std::isalnum((unsigned char)body[j])||body[j]=='_')) j++;
            std::string id = body.substr(i, j-i);
            auto it = m.find(id);
            out += (it != m.end()) ? it->second : id;
            i = j; continue;
        }
        out += c; i++;
    }
    return out;
}

// Error state shared by one top-level ppExpand call and its nested expansions: the
// first malformed macro invocation (arity mismatch, unterminated argument list) and
// the column of the top-level token whose expansion produced it.
struct PPExpandCtx {
    std::string err;
    size_t errCol = 0;
    size_t topCol = 0;
    int depth = 0;
    bool openCall = false;
    // A function-like macro name ends the top-level text (only blanks or a comment
    // after it): its `(` may start a following line, as in C.
    bool trailingFnName = false;
};

static bool ppIdentStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
static bool ppIdentChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

// Skip spaces, tabs and closed `/* */` comments on one line starting at p.
static size_t ppSkipBlank(const std::string& s, size_t p) {
    size_t n = s.size();
    while (p < n) {
        if (s[p] == ' ' || s[p] == '\t') { p++; continue; }
        if (s[p] == '/' && p + 1 < n && s[p + 1] == '*') {
            size_t e = s.find("*/", p + 2);
            if (e == std::string::npos) return p;
            p = e + 2; continue;
        }
        break;
    }
    return p;
}

// Like ppSkipBlank, but also across newlines and `//` comments (a function-like
// macro's `(` may follow its name on a later line, as in C).
static size_t ppSkipBlankNL(const std::string& s, size_t p) {
    size_t n = s.size();
    while (p < n) {
        p = ppSkipBlank(s, p);
        if (p < n && s[p] == '\n') { p++; continue; }
        if (p + 1 < n && s[p] == '/' && s[p + 1] == '/') {
            size_t e = s.find('\n', p);
            if (e == std::string::npos) return n;
            p = e + 1; continue;
        }
        break;
    }
    return p;
}

// Expand all macros in `text`, recursively. `expanding` guards against a macro
// re-expanding within its own expansion (prevents infinite loops). `inBlock`, when
// given, carries `/* ... */` comment state across source lines: comment text is
// copied verbatim (an apostrophe in a comment must not open a char literal).
// `ctx`, when given, receives the first malformed-invocation error. A replacement
// that ends in the name of a function-like macro is rescanned together with the
// rest of the text, so `#define CALLF F` makes `CALLF(2)` call F (C semantics).
static std::string ppExpand(const std::string& input,
                            const std::map<std::string, Macro>& defines,
                            std::set<std::string>& expanding,
                            bool* inBlock = nullptr,
                            PPExpandCtx* ctx = nullptr) {
    bool blk = inBlock ? *inBlock : false;
    std::string text = input;
    long long colShift = 0;
    size_t keepTop = 0;
    std::string out; size_t i = 0, n = text.size();
    auto fail = [&](const std::string& msg) {
        if (ctx && ctx->err.empty()) { ctx->err = msg; ctx->errCol = ctx->topCol; }
    };
    auto failOpen = [&](const std::string& msg) {
        if (ctx && ctx->depth == 0 && ctx->err.empty()) ctx->openCall = true;
        fail(msg);
    };
    while (i < n) {
        char c = text[i];
        if (blk) {
            size_t e = text.find("*/", i);
            if (e == std::string::npos) { out += text.substr(i); i = n; break; }
            out += text.substr(i, e + 2 - i); i = e + 2; blk = false;
            continue;
        }
        if (c == '/' && i+1<n && text[i+1]=='*') { out += "/*"; i += 2; blk = true; continue; }
        if (c == '"' || c == '\'') {
            ppCopyLiteral(text, i, out);
            continue;
        }
        if (c == '/' && i+1<n && text[i+1]=='/') { out += text.substr(i); break; }
        if (ppIdentStart(c)) {
            size_t j = i; while (j<n && ppIdentChar(text[j])) j++;
            std::string id = text.substr(i, j-i);
            if (ctx && ctx->depth == 0 && i >= keepTop) ctx->topCol = (size_t)((long long)i + colShift + 1);
            auto it = defines.find(id);
            if (it != defines.end() && !expanding.count(id)) {
                const Macro& mac = it->second;
                std::string res;
                size_t after = j;
                bool expanded = false;
                if (!mac.isFunction) {
                    expanding.insert(id);
                    if (ctx) ctx->depth++;
                    res = ppExpand(mac.body, defines, expanding, nullptr, ctx);
                    if (ctx) ctx->depth--;
                    expanding.erase(id);
                    expanded = true;
                } else {
                    size_t k = ppSkipBlankNL(text, j);
                    if (k >= n && ctx && ctx->depth == 0) ctx->trailingFnName = true;
                    if (k < n && text[k] == '(') {           // function-like call
                        // Split the arguments at top-level commas. String and char
                        // literals are copied whole, so a ',' or ')' inside one does
                        // not end an argument; a comment counts as a space.
                        std::vector<std::string> args; std::string cur; int depth = 0;
                        bool sawAny = false, closed = false; size_t p = k + 1;
                        while (p < n) {
                            char d = text[p];
                            if (d == '"' || d == '\'') { ppCopyLiteral(text, p, cur); sawAny = true; continue; }
                            if (d == '/' && p + 1 < n && text[p + 1] == '*') {
                                size_t e = text.find("*/", p + 2);
                                if (e == std::string::npos) { p = n; break; }
                                cur += ' '; p = e + 2; continue;
                            }
                            if (d == '/' && p + 1 < n && text[p + 1] == '/') {
                                size_t e = text.find('\n', p);
                                if (e == std::string::npos) { p = n; break; }
                                p = e; continue;
                            }
                            if (d == '\n') { cur += ' '; p++; continue; }
                            if (d == '(') { depth++; cur += d; sawAny = true; }
                            else if (d == ')') { if (depth==0) { p++; closed = true; break; } depth--; cur += d; }
                            else if (d == ',' && depth==0) { args.push_back(ppTrim(cur)); cur.clear(); sawAny = true; }
                            else { cur += d; if (d != ' ' && d != '\t') sawAny = true; }
                            p++;
                        }
                        if (!closed) {
                            failOpen("unterminated argument list invoking macro '" + id + "'");
                            out += id; i = j; continue;
                        }
                        if (sawAny) args.push_back(ppTrim(cur));
                        if (args.empty() && mac.params.size() == 1) args.push_back("");
                        if (args.size() != mac.params.size()) {
                            size_t want = mac.params.size();
                            if (args.size() < want)
                                fail("macro '" + id + "' requires " + std::to_string(want) + " argument" +
                                     (want == 1 ? "" : "s") + ", but only " + std::to_string(args.size()) + " given");
                            else
                                fail("macro '" + id + "' passed " + std::to_string(args.size()) + " argument" +
                                     (args.size() == 1 ? "" : "s") + ", but takes just " + std::to_string(want));
                            out += text.substr(i, p - i); i = p; continue;
                        }
                        // Arguments are fully macro-expanded before substitution (C
                        // rule), so a nested call like F(F(3)) expands the inner one.
                        if (ctx) ctx->depth++;
                        for (auto& a : args) a = ppExpand(a, defines, expanding, nullptr, ctx);
                        std::string sub = ppSubstParams(mac.body, mac.params, args);
                        expanding.insert(id);
                        res = ppExpand(sub, defines, expanding, nullptr, ctx);
                        expanding.erase(id);
                        if (ctx) ctx->depth--;
                        after = p;
                        expanded = true;
                    }
                    // function-like name not followed by '(' → leave as-is
                }
                if (expanded) {
                    // A replacement ending in a function-like macro name followed by
                    // '(' in the remaining text: rescan that name with the rest.
                    size_t t = res.size();
                    while (t > 0 && (res[t-1] == ' ' || res[t-1] == '\t')) t--;
                    size_t s0 = t;
                    while (s0 > 0 && ppIdentChar(res[s0-1])) s0--;
                    if (s0 < t && ppIdentStart(res[s0])) {
                        std::string tid = res.substr(s0, t - s0);
                        auto ft = defines.find(tid);
                        size_t k2 = ppSkipBlankNL(text, after);
                        bool fnName = ft != defines.end() && ft->second.isFunction && !expanding.count(tid);
                        if (fnName && k2 >= n && ctx && ctx->depth == 0) ctx->trailingFnName = true;
                        if (fnName && k2 < n && text[k2] == '(') {
                            out += res.substr(0, s0);
                            colShift += (long long)after - (long long)tid.size();
                            text = tid + text.substr(after);
                            keepTop = tid.size();
                            i = 0; n = text.size();
                            continue;
                        }
                    }
                    out += res; i = after; continue;
                }
            }
            out += id; i = j; continue;
        }
        out += c; i++;
    }
    if (inBlock) *inBlock = blk;
    return out;
}

// Does a line ending in '\' genuinely continue onto the next physical line?
// Only if the trailing '\' is real code — NOT inside a // or /* */ comment, nor a
// string/char literal. Otherwise a comment ending in '\' would silently swallow
// the following source line (a footgun: it eats a `return`, an `else`, etc.).
// Note: cross-line block-comment state isn't tracked here (a '\' on an interior
// line of a multi-line /* */ may still splice — harmless, it only drops a newline
// inside comment text). On a directive line (`directive`) a '\' inside a string or char
// literal splices too, as in C, so a #define body's literal may span lines.
// Precondition: line.back() == '\\'.
static bool backslashContinuesLine(const std::string& line, bool directive, bool startInStr) {
    bool inStr = startInStr, inChr = false, inBlock = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inBlock) {
            if (c == '*' && i + 1 < line.size() && line[i + 1] == '/') { inBlock = false; ++i; }
            continue;
        }
        if (inStr) {
            if (c == '\\' && i + 1 < line.size()) { ++i; continue; }   // skip escaped char
            if (c == '"') inStr = false;
            continue;
        }
        if (inChr) {
            if (c == '\\' && i + 1 < line.size()) { ++i; continue; }
            if (c == '\'') inChr = false;
            continue;
        }
        if (c == '"')  { inStr = true; continue; }
        if (c == '\'') { inChr = true; continue; }
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') return false;  // line comment
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') { inBlock = true; ++i; continue; }
    }
    // The trailing '\' is reached in this state: code context splices (and a literal
    // on a directive line).
    return !inBlock && (directive || (!inStr && !inChr));
}

// Where a string literal open at the start of `line` closes: the index just past its
// closing quote, or npos when it continues onto the next line (the lexer's string
// literals may span lines).
static size_t ppStringClose(const std::string& line) {
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\') { ++i; continue; }
        if (line[i] == '"') return i + 1;
    }
    return std::string::npos;
}

// Does `text` (code, starting in a `/* */` comment when `blk`) end inside a string
// literal? Scanned in the order ppExpand applies: comments, then literals, then `//`.
static bool ppEndsInString(const std::string& text, bool blk) {
    size_t n = text.size();
    for (size_t i = 0; i < n; ++i) {
        char c = text[i];
        if (blk) {
            if (c == '*' && i + 1 < n && text[i + 1] == '/') { blk = false; ++i; }
            continue;
        }
        if (c == '/' && i + 1 < n && text[i + 1] == '*') { blk = true; ++i; continue; }
        if (c == '/' && i + 1 < n && text[i + 1] == '/') {
            size_t e = text.find('\n', i);
            if (e == std::string::npos) return false;
            i = e; continue;
        }
        if (c == '"' || c == '\'') {
            char q = c; ++i;
            while (i < n && text[i] != q) { if (text[i] == '\\') ++i; ++i; }
            if (i >= n) return q == '"';
        }
    }
    return false;
}

// Scan `line` for comment state: starting inside a `/* */` comment when `inBlock`,
// return whether the line ends inside one. String/char literals and `//` comments
// are skipped, in the same order ppExpand applies.
static bool endsInBlockComment(const std::string& line, bool inBlock) {
    size_t n = line.size();
    for (size_t i = 0; i < n; ++i) {
        char c = line[i];
        if (inBlock) {
            if (c == '*' && i + 1 < n && line[i + 1] == '/') { inBlock = false; ++i; }
            continue;
        }
        if (c == '/' && i + 1 < n && line[i + 1] == '*') { inBlock = true; ++i; continue; }
        if (c == '"' || c == '\'') {
            char q = c; ++i;
            while (i < n && line[i] != q) { if (line[i] == '\\') ++i; ++i; }
            continue;
        }
        if (c == '/' && i + 1 < n && line[i + 1] == '/') break;
    }
    return inBlock;
}
