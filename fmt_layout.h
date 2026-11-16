// fmt_layout.h: the statement layout behind formatSource (internal to fmt.cpp, which
// includes it once).
#pragma once
#include <cctype>
#include <string>
#include <vector>

// ── Statement layout for `eskiuc fmt` ───────────────────────────────────────
// The braces and parentheses open across lines: each `{` is a block ('B': a function
// or control-flow body, a struct or interface body, a match arm) or an initializer
// ('I': an array or struct literal, an enum body), and each brace level counts its own
// open `(` and `[`.
namespace {
struct FmtLayout {
    std::vector<char> kinds;
    std::vector<int> parens;
    int paren = 0;
};

bool fmtIdentChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

std::string fmtFirstWord(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    size_t j = i;
    while (j < s.size() && fmtIdentChar(s[j])) j++;
    return s.substr(i, j - i);
}

// Count the brackets of t[from..] (strings and chars skipped, up to a comment) into the
// layout state: a line that is left as it is, or the code after a multi-line string.
void fmtCountBrackets(FmtLayout& st, const std::string& t, size_t from) {
    size_t n = t.size();
    for (size_t i = from; i < n; ++i) {
        char c = t[i];
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < n && t[j] != c) { if (t[j] == '\\' && j + 1 < n) j++; j++; }
            i = j;
            continue;
        }
        if (c == '/' && i + 1 < n && (t[i + 1] == '/' || t[i + 1] == '*')) break;
        if (c == '(' || c == '[') {
            st.paren++;
        } else if (c == ')' || c == ']') {
            st.paren--;
        } else if (c == '{') {
            st.kinds.push_back('I');
            st.parens.push_back(st.paren);
            st.paren = 0;
        } else if (c == '}' && !st.kinds.empty()) {
            st.kinds.pop_back();
            st.paren = st.parens.back();
            st.parens.pop_back();
        }
    }
}

// The keywords, which never name a generic type or function (`return a < b > c`).
const char* const kFmtKeywords =
    " let int float double bool char string void struct packed union interface enum fn operator asm"
    " volatile static escaping must_use async await const sizeof free_closure thread_create thread_join"
    " for in while do if else switch match case default break return import extern intrinsic alloc_with"
    " null true false try catch finally throw defer errdefer continue uint ";

bool fmtKeyword(const std::string& w) {
    return std::strstr(kFmtKeywords, (" " + w + " ").c_str()) != nullptr;
}

// A type argument list `<...>` starting at t[p] == '<': the index of its closing `>`, or
// npos when t[p..] is not one. Its tokens may only be identifiers, `*`, `?`, `,`, an
// array suffix `[N]` (identifiers and digits inside), `fn(...)` parameter lists with a
// `->` after the `)`, and nested lists after an identifier; it closes on the same line.
// A `*` between two operands (`b * c`) is a product, not a type. The positions of the
// `<` and `>` of this list and of the nested ones are added to `marks`.
size_t fmtTypeArgEnd(const std::string& t, size_t p, std::vector<size_t>& marks) {
    size_t n = t.size(), i = p + 1;
    if (i < n && (t[i] == '<' || t[i] == '=')) return std::string::npos;
    int paren = 0;
    bool brack = false, any = false;
    char prev = '<';               // the previous token: 'w' a word, 'a' `->`, else itself
    std::string word;
    while (i < n) {
        char c = t[i];
        if (c == ' ' || c == '\t') { i++; continue; }
        if (std::isalpha((unsigned char)c) || c == '_' || (brack && std::isdigit((unsigned char)c))) {
            size_t j = i;
            while (j < n && fmtIdentChar(t[j])) j++;
            word = t.substr(i, j - i);
            prev = 'w';
            any = true;
            i = j;
            continue;
        }
        if (c == '<') {
            if (prev != 'w' || brack) return std::string::npos;
            size_t e = fmtTypeArgEnd(t, i, marks);
            if (e == std::string::npos) return e;
            prev = '>';
            i = e + 1;
            continue;
        }
        if (c == '>') {
            if (paren > 0 || brack || !any || !(prev == 'w' || prev == '*' || prev == ']' || prev == ')' || prev == '>'))
                return std::string::npos;
            marks.push_back(p);
            marks.push_back(i);
            return i;
        }
        if (c == '-') {
            if (prev != ')' || i + 1 >= n || t[i + 1] != '>') return std::string::npos;
            prev = 'a';
            i += 2;
            continue;
        }
        if (c == '(') {
            if (brack || !(prev == 'w' && word == "fn")) return std::string::npos;
            paren++;
        } else if (c == ')') {
            if (brack || paren == 0) return std::string::npos;
            paren--;
        } else if (c == '[') {
            if (brack || !(prev == 'w' || prev == ']' || prev == '>' || prev == '*')) return std::string::npos;
            brack = true;
        } else if (c == ']') {
            if (!brack) return std::string::npos;
            brack = false;
        } else if (c == '*') {
            size_t j = i + 1;
            while (j < n && (t[j] == ' ' || t[j] == '\t')) j++;
            bool operandAfter = j < n && (fmtIdentChar(t[j]) || t[j] == '(');
            if (brack || ((prev == 'w' || prev == ']' || prev == '>' || prev == ')') && operandAfter))
                return std::string::npos;
        } else if (c == '?' || c == ',') {
            if (brack) return std::string::npos;
        } else {
            return std::string::npos;
        }
        prev = c;
        i++;
    }
    return std::string::npos;
}

// The `<` at t[i] opens a type argument list beyond doubt: it follows an identifier
// that is not a keyword, directly or, for a capitalized name (`List < int >`), after
// blanks (`a < b > c` and `f(a < b, c > (d))` are comparisons); its tokens are type
// tokens (fmtTypeArgEnd); and the token after its `>` follows a type or a generic call:
// an identifier (a declaration), `(` (a call), `{` (a struct literal or body), `)` `,`
// `;` `[` or `*`. The `<` and `>` positions of it and of its nested lists go into `marks`.
bool fmtTypeArgs(const std::string& t, size_t i, const std::string& head, std::vector<size_t>& marks) {
    if (head.empty() || std::isdigit((unsigned char)head[0]) || fmtKeyword(head)) return false;
    size_t k = i;
    while (k > 0 && (t[k - 1] == ' ' || t[k - 1] == '\t')) k--;
    if (k == 0 || !fmtIdentChar(t[k - 1])) return false;
    if (k < i && !(head[0] >= 'A' && head[0] <= 'Z')) return false;
    std::vector<size_t> found;
    size_t e = fmtTypeArgEnd(t, i, found);
    if (e == std::string::npos) return false;
    size_t j = e + 1;
    while (j < t.size() && (t[j] == ' ' || t[j] == '\t')) j++;
    if (j >= t.size()) return false;
    char f = t[j];
    if (!(std::isalpha((unsigned char)f) || f == '_' || std::strchr("({),;[*", f))) return false;
    marks.insert(marks.end(), found.begin(), found.end());
    return true;
}

// Operators that get one space on each side, longest first.
const char* const kFmtSpacedOps[] = {"<<=", ">>=", "==", "!=", "<=", ">=", "&&", "||", "+=", "-=",
                                     "*=", "/=", "%=", "&=", "|=", "^=", "="};

// Lay out one code line `t` (no leading blanks): split it after each statement's `;`
// and around block braces, so `if (a) { return 1; }` becomes three lines and
// `x = 1; y = 2;` two, and normalize its spacing (one space between tokens, around
// assignment, comparison and logical operators, after a comma or semicolon and before
// a block `{`; none before `,` `;` `)` `]` or after `(` `[`). Only blanks between tokens
// change, so the tokens are the same. `last` is the last code character of the
// previous line, and is updated. A line that ends inside a string or a comment, or with
// `\`, is left as it is (its braces and parentheses still count). A trailing `//`
// comment stays on the first piece.
std::vector<std::string> fmtLayoutLine(FmtLayout& st, const std::string& t, char& last) {
    if (t.find('\0') != std::string::npos) return {t};   // a NUL byte: left alone
    std::vector<std::string> pieces;
    std::string cur, comment;
    bool needSpace = false;
    std::string lastWord;
    std::vector<size_t> typeMarks;   // the `<` and `>` of the type argument lists found so far
    size_t n = t.size(), i = 0;
    bool ok = n == 0 || t[n - 1] != '\\';
    auto rtrimCur = [&] { while (!cur.empty() && cur.back() == ' ') cur.pop_back(); };
    auto skipBlanks = [&](size_t j) { while (j < n && (t[j] == ' ' || t[j] == '\t')) j++; return j; };
    auto restIsCode = [&](size_t j) {
        j = skipBlanks(j);
        return j < n && !(t[j] == '/' && j + 1 < n && (t[j + 1] == '/' || t[j + 1] == '*'));
    };
    auto emit = [&](const std::string& s) {
        if (needSpace && !cur.empty() && cur.back() != ' ') cur += ' ';
        needSpace = false;
        cur += s;
        last = s.back();
    };
    auto flush = [&] {
        rtrimCur();
        if (!cur.empty()) pieces.push_back(cur);
        cur.clear();
        needSpace = false;
        lastWord.clear();
    };
    auto openBrace = [&](char kind) {
        st.kinds.push_back(kind);
        st.parens.push_back(st.paren);
        st.paren = 0;
    };
    // After a block's `}`: `else`, `while`, `catch`, `finally` and `;` `)` `,` `]` stay
    // on its line; anything else starts a new one.
    auto afterClose = [&] {
        size_t j = skipBlanks(i);
        if (j >= n) return;
        size_t k = j;
        while (k < n && fmtIdentChar(t[k])) k++;
        std::string w = t.substr(j, k - j);
        if (w == "else" || w == "while" || w == "catch" || w == "finally") { cur += ' '; i = j; return; }
        if (t[j] == ';' || t[j] == ')' || t[j] == ',' || t[j] == ']') { i = j; return; }
        if (restIsCode(j)) flush();
    };
    while (ok && i < n) {
        char c = t[i];
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < n && t[j] != c) { if (t[j] == '\\' && j + 1 < n) j++; j++; }
            if (j >= n) { ok = false; break; }
            emit(t.substr(i, j + 1 - i));
            lastWord.clear();
            i = j + 1;
            continue;
        }
        if (c == '/' && i + 1 < n && t[i + 1] == '/') {
            size_t g = i;
            while (g > 0 && (t[g - 1] == ' ' || t[g - 1] == '\t')) g--;
            comment = t.substr(g);
            break;
        }
        if (c == '/' && i + 1 < n && t[i + 1] == '*') {
            size_t e = t.find("*/", i + 2);
            if (e == std::string::npos) { ok = false; break; }
            char keep = last;
            emit(t.substr(i, e + 2 - i));
            last = keep;
            i = e + 2;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {   // a lone CR is a blank to the lexer
            if (!cur.empty() && cur.back() != ' ' && cur.back() != '(' && cur.back() != '[') cur += ' ';
            i++;
            continue;
        }
        if (fmtIdentChar(c)) {
            size_t j = i;
            while (j < n && fmtIdentChar(t[j])) j++;
            std::string w = t.substr(i, j - i);
            emit(w);
            i = j;
            if ((w == "if" || w == "while" || w == "for" || w == "switch" || w == "catch" || w == "match") &&
                i < n && t[i] == '(' && lastWord != "operator")
                cur += ' ';
            lastWord = w;
            continue;
        }
        // A type argument list: no blank around its `<` or before its `>` (`List<int>`),
        // except between two `>` written apart, which would otherwise become `>>`.
        if (c == '<' || c == '>') {
            bool marked = std::find(typeMarks.begin(), typeMarks.end(), i) != typeMarks.end();
            if (!marked && c == '<' && lastWord != "operator") marked = fmtTypeArgs(t, i, lastWord, typeMarks);
            if (marked) {
                bool apart = !cur.empty() && cur.back() == ' ';
                rtrimCur();
                if (c == '>' && apart && !cur.empty() && cur.back() == '>') cur += ' ';
                cur += c;
                last = c;
                needSpace = false;
                lastWord.clear();
                i = c == '<' ? skipBlanks(i + 1) : i + 1;
                continue;
            }
        }
        if (lastWord == "operator" && std::strchr("+-*/%=!<>&|^~[]", c)) {   // `operator ==` as written
            size_t j = i;
            while (j < n && std::strchr("+-*/%=!<>&|^~[]", t[j])) j++;
            emit(t.substr(i, j - i));
            lastWord.clear();
            i = j;
            continue;
        }
        const char* op = nullptr;
        if (lastWord != "operator")
            for (const char* o : kFmtSpacedOps)
                if (t.compare(i, std::strlen(o), o) == 0) { op = o; break; }
        if (op) {
            rtrimCur();
            if (!cur.empty()) cur += ' ';
            cur += op;
            last = '=';
            needSpace = true;
            lastWord.clear();
            i += std::strlen(op);
            continue;
        }
        if (c == ',') {
            rtrimCur();
            cur += ',';
            last = ',';
            needSpace = true;
            lastWord.clear();
            i++;
            continue;
        }
        if (c == ';') {
            rtrimCur();
            cur += ';';
            last = ';';
            lastWord.clear();
            i++;
            if (st.paren == 0 && (st.kinds.empty() || st.kinds.back() == 'B') && restIsCode(i)) flush();
            else needSpace = true;
            continue;
        }
        if (c == '(' || c == '[') {
            emit(std::string(1, c));
            st.paren++;
            lastWord.clear();
            i++;
            continue;
        }
        if (c == ')' || c == ']') {
            rtrimCur();
            cur += c;
            last = c;
            needSpace = false;
            st.paren--;
            lastWord.clear();
            i++;
            continue;
        }
        if (c == '{') {
            std::string head = cur;
            while (!head.empty() && head.back() == ' ') head.pop_back();
            std::string fw = fmtFirstWord(head);
            bool topBlock = st.kinds.empty() || st.kinds.back() == 'B';
            bool block;
            if (head.empty())
                block = topBlock && st.paren == 0 && last != '=' && last != ',' && last != '(' && last != '[';
            else if (fw == "enum")
                block = false;
            else
                block = head.back() == ')' || (head.size() >= 2 && head.compare(head.size() - 2, 2, "->") == 0) ||
                        lastWord == "else" || lastWord == "do" || lastWord == "try" || lastWord == "finally" ||
                        lastWord == "defer" || lastWord == "errdefer" || fw == "struct" || fw == "union" ||
                        fw == "interface" || fw == "packed" || fw == "match" ||
                        (head.back() == ':' && (fw == "case" || fw == "default"));
            lastWord.clear();
            if (!block) {
                emit("{");
                openBrace('I');
                i++;
                continue;
            }
            rtrimCur();
            if (!cur.empty()) cur += ' ';
            size_t j = skipBlanks(i + 1);
            if (j < n && t[j] == '}') {          // an empty block stays `{}`
                cur += "{}";
                last = '}';
                needSpace = false;
                i = j + 1;
                afterClose();
                continue;
            }
            cur += '{';
            last = '{';
            needSpace = false;
            openBrace('B');
            i++;
            if (restIsCode(i)) flush();
            continue;
        }
        if (c == '}') {
            char kind = 'I';
            if (!st.kinds.empty()) {
                kind = st.kinds.back();
                st.kinds.pop_back();
                st.paren = st.parens.back();
                st.parens.pop_back();
            }
            i++;
            lastWord.clear();
            needSpace = false;
            if (kind == 'B') {
                flush();
                cur = "}";
                last = '}';
                afterClose();
            } else {
                cur += '}';
                last = '}';
            }
            continue;
        }
        emit(std::string(1, c));
        lastWord.clear();
        i++;
    }
    if (!ok) {
        fmtCountBrackets(st, t, i);         // the rest still counts; the text is kept
        return {t};
    }
    rtrimCur();
    if (!cur.empty()) pieces.push_back(cur);
    // A trailing comment stays with the statement's first line (`while (x) {  // why`).
    if (!comment.empty()) {
        if (pieces.empty()) pieces.push_back(comment.substr(comment.find_first_not_of(" \t")));
        else pieces.front() += comment;
    }
    if (pieces.empty()) pieces.push_back(t);
    return pieces;
}
}  // namespace
