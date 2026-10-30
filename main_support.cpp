#include <algorithm>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <climits>
#include <set>
#include <cerrno>
#include <filesystem>
#ifndef _WIN32
  #include <unistd.h>
  #include <sys/wait.h>
#endif
#ifdef __APPLE__
  #include <mach-o/dyld.h>
#elif defined(__linux__)
  #include <unistd.h>
#elif defined(_WIN32)
  // Declare just GetModuleFileNameA instead of including <windows.h>, whose macros
  // (VOID, CONST, TRUE, FALSE, FLOAT, IN, INTERFACE, ...) collide with our TokenType
  // enum. Plain types: HMODULE = void*, LPSTR = char*, DWORD = unsigned long.
  extern "C" __declspec(dllimport) unsigned long __stdcall
      GetModuleFileNameA(void* hModule, char* lpFilename, unsigned long nSize);
#endif
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Object/ArchiveWriter.h"
#include "llvm/Support/Path.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/TargetParser/Triple.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "ast/ast_printer.h"
#include "sema/type_checker.h"
#include "sema/async_transform.h"
#include "codegen/codegen.h"
#include "main_support.h"
#include "pkg.h"

// Definition of the shared stdlib-root global (declared in main_support.h).
std::string stdlibRoot;

// Resolve stdlib root: $ESKIU_ROOT env var, or dirname(argv[0])/../lib/eskiu
std::string resolveStdlibPath() {
    const char* env = std::getenv("ESKIU_ROOT");
    if (env && *env) return std::string(env);

    // Deduce from binary location
    char buf[4096] = {};
#ifdef __APPLE__
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
#elif defined(__linux__)
    if (readlink("/proc/self/exe", buf, sizeof(buf) - 1) > 0) {
#elif defined(_WIN32)
    if (GetModuleFileNameA(nullptr, buf, (unsigned long)sizeof(buf)) > 0) {
#else
    if (false) {
#endif
        std::string binPath(buf);
        // Accept either separator: GetModuleFileNameA returns backslashes.
        size_t slash = binPath.find_last_of("/\\");
        if (slash != std::string::npos) {
            // binary is at <prefix>/bin/eskiuc → look for <prefix>/lib/eskiu
            std::string binDir = binPath.substr(0, slash);
            size_t parentSlash = binDir.find_last_of("/\\");
            if (parentSlash != std::string::npos) {
                std::string prefix = binDir.substr(0, parentSlash);
                std::string candidate = prefix + "/lib/eskiu";
                std::ifstream probe(candidate + "/stdlib/result.esk");
                if (probe.good()) return candidate;
            }
            // Also try sibling directory (dev: build/ next to stdlib/)
            std::string devCandidate = binDir + "/..";
            std::ifstream probe2(devCandidate + "/stdlib/result.esk");
            if (probe2.good()) return devCandidate;
        }
    }
    return "";
}

// Read file contents into string
std::string readFile(const std::string& filename) {
    std::error_code ec;
    if (std::filesystem::is_directory(filename, ec)) {
        std::cerr << "error: '" << filename << "' is a directory, not a source file" << std::endl;
        exit(1);
    }
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "error: could not open file '" << filename << "'" << std::endl;
        exit(1);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Directory portion of a path, or "." if there is no slash.
std::string dirOf(const std::string& path) {
    size_t slash = path.rfind('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

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

// ── eskiuc fmt ──────────────────────────────────────────────────────────────
// A comment-preserving formatter that only changes blanks between tokens and line
// breaks, so it can never change a program's meaning:
//   * one statement per line: a line is split after each statement's `;` and around
//     block braces (fmtLayoutLine), so `if (a) { return 1; }` takes three lines;
//     initializer braces (`{1, 2}`, `P{x: 1}`, an enum body) stay on their line
//   * spacing: one blank between tokens, around assignment, comparison and logical
//     operators, after `,` and `;`, before a block `{`; none before `,` `;` `)` `]`
//   * type argument lists: no blank around `<` or before `>` (`List < int >` becomes
//     `List<int>`), only for a `<...>` that cannot be a comparison (fmtTypeArgs); two
//     `>` written apart keep their blank, so they never become `>>`
//   * long lines are not broken
//   * indentation = 4 spaces per `{`-nesting level; a line that continues a statement
//     (inside open parentheses, or after a line that does not end one) keeps its
//     indentation relative to the statement's first line, at least 4 more
//   * trailing whitespace stripped, except after a trailing `\` (trimming it there
//     would turn the line into a continuation)
//   * blank lines kept; only blank lines at the end of the file are dropped
//   * exactly one final newline
//   * a line after one ending in `\` (a continuation), a line inside a multi-line string
//     or block comment, and a line that opens one keep their bytes
// Comments and every byte of a string or char literal are preserved; only the `\r` of
// a CRLF line ending is taken off. Braces inside strings, char literals and comments are
// ignored, so formatting is idempotent and safe. Preprocessor lines (`#…`) sit at
// column 0 and do not affect nesting. The file's line ending (LF or CRLF, judged by its
// first line break) is preserved.
std::string formatSource(const std::string& src) {
    size_t firstNl = src.find('\n');
    const std::string eol = (firstNl != std::string::npos && firstNl > 0 && src[firstNl - 1] == '\r')
                                ? "\r\n" : "\n";
    std::vector<std::string> lines;
    { std::string cur;
      for (char c : src) {
          if (c != '\n') { cur += c; continue; }
          if (!cur.empty() && cur.back() == '\r') cur.pop_back();
          lines.push_back(cur); cur.clear();
      }
      lines.push_back(cur); }
    // The text after the final newline is a line only when it is not empty: a file
    // ending inside a string or comment would otherwise gain a blank line on each run.
    if (lines.size() > 1 && lines.back().empty()) lines.pop_back();

    auto rtrim = [](const std::string& s) {
        size_t b = s.find_last_not_of(" \t");
        if (b == std::string::npos) return std::string();
        if (s[b] == '\\') b = s.size() - 1;       // `\ ` must not become a continuation
        return s.substr(0, b + 1);
    };

    std::string out;
    int depth = 0;            // current `{` nesting
    bool inBlock = false;     // inside a /* … */ block comment
    int pendingBlank = 0;     // blank lines buffered (dropped only at the end of the file)
    bool inStr = false;       // a string literal continues onto the next line
    bool prevCont = false;    // the previous line ends with `\`: this one continues it
    bool ppCont = false;      // the continued line is a preprocessor line (no nesting)
    // The preprocessor's view, which decides what the lexer sees: a `#` line inside a
    // multi-line string is string text, only one branch of a conditional reaches the
    // lexer (so each branch, and the code after `#endif`, starts from the string state
    // at the `#if`), and a directive that leaves a `/*` open opens a comment for the
    // following lines. A string opened inside a conditional may be in a branch that is
    // not compiled (a stray quote in `#if 0`), so there a `#` line keeps its bytes but
    // still counts as a directive.
    std::vector<bool> condStr;
    bool strSeen = false;     // inStr was already set at the start of the previous line
    bool strInCond = false;   // the open string began inside a conditional
    std::string ppLine;       // the directive's logical line (continuations joined)
    FmtLayout layout;         // the open braces' kinds and parentheses (fmtLayoutLine)
    char last = 0;            // the last code character so far
    std::vector<char> condLast;   // `last` at each open #if
    // The nesting at each open #if: each branch starts from it (only one reaches the
    // lexer), and the code after `#endif` continues from the last branch.
    std::vector<int> condDepth;
    std::vector<FmtLayout> condLayout;
    int stmtOrig = 0, stmtNew = 0;   // the current statement's first line: indent before and after
    bool sepNext = false;     // the last line closed a top-level block
    bool firstLine = true;
    auto directiveOpensComment = [](const std::string& l) {
        size_t n = l.size();
        for (size_t i = 0; i < n; ++i) {
            char c = l[i];
            if (c == '"' || c == '\'') {
                for (++i; i < n && l[i] != c; ++i) if (l[i] == '\\' && i + 1 < n) ++i;
                continue;
            }
            if (c == '/' && i + 1 < n && l[i + 1] == '/') return false;
            if (c == '/' && i + 1 < n && l[i + 1] == '*') {
                size_t e = l.find("*/", i + 2);
                if (e == std::string::npos) return true;
                i = e + 1;
            }
        }
        return false;
    };

    // Update nesting from t[from..] (code state), skipping strings/chars/comments.
    // Strings are checked first, so a `/*` or `}` inside a literal is ignored. A
    // string still open at the end of the line leaves inStr set.
    auto scanNesting = [&](const std::string& t, size_t from) {
        for (size_t i = from; i < t.size(); ++i) {
            char c = t[i];
            if (inStr || c == '"' || c == '\'') {                            // string / char literal
                char q = inStr ? '"' : c;
                if (!inStr) ++i;
                inStr = false;
                while (i < t.size() && t[i] != q) {
                    if (t[i] == '\\' && i + 1 < t.size()) { ++i; }           // skip the escaped char
                    ++i;
                }
                if (i >= t.size() && q == '"') inStr = true;
                continue;
            }
            if (c == '/' && i + 1 < t.size() && t[i + 1] == '/') break;       // line comment
            if (c == '/' && i + 1 < t.size() && t[i + 1] == '*') {            // block comment
                inBlock = true;
                for (size_t j = i + 2; j + 1 < t.size(); ++j)
                    if (t[j] == '*' && t[j + 1] == '/') { inBlock = false; i = j + 1; break; }
                if (inBlock) break;                                          // runs onto next line
                continue;
            }
            if (c == '{') depth++;
            else if (c == '}') depth--;
        }
        if (depth < 0) depth = 0;
    };

    for (const std::string& raw : lines) {
        bool cont = prevCont;
        bool shebang = firstLine && raw.compare(0, 2, "#!") == 0;
        firstLine = false;
        prevCont = !raw.empty() && raw.back() == '\\';
        if (inStr && !strSeen) strInCond = !condStr.empty();
        strSeen = inStr;
        size_t h0 = raw.find_first_not_of(" \t");
        bool directive = !inBlock && !cont && (!inStr || strInCond) && h0 != std::string::npos && raw[h0] == '#';
        if (inBlock) {                       // verbatim until the comment closes
            out += raw; out += eol;
            for (size_t i = 0; i + 1 < raw.size(); ++i)
                if (raw[i] == '*' && raw[i + 1] == '/') {
                    inBlock = false;
                    scanNesting(raw, i + 2);         // code after the `*/` still nests
                    break;
                }
            continue;
        }
        if (inStr && !directive) {           // inside a multi-line string: bytes verbatim
            std::string t = raw;
            scanNesting(t, 0);
            if (!inStr) {
                t = rtrim(t);
                if (!t.empty()) last = t.back();
                size_t q = 0;                    // the string's closing quote
                while (q < t.size() && t[q] != '"') { if (t[q] == '\\' && q + 1 < t.size()) q++; q++; }
                fmtCountBrackets(layout, t, q + 1);
            }
            out += t; out += eol;
            continue;
        }
        if (cont) {                          // a continuation line: bytes verbatim
            out += raw; out += eol;
            if (!ppCont) scanNesting(raw, 0);
            else if (prevCont) ppLine += raw.substr(0, raw.size() - 1);
            else inBlock = directiveOpensComment(ppLine + raw);
            continue;
        }
        size_t a = raw.find_first_not_of(" \t");
        if (a == std::string::npos) { pendingBlank++; continue; }
        std::string t = raw.substr(a);

        // A top-level definition is set apart from what follows it.
        if (sepNext && pendingBlank == 0 && t[0] != '}' && t[0] != '#') out += eol;
        sepNext = false;
        for (; pendingBlank > 0; pendingBlank--) out += eol;   // keep every blank line

        ppCont = t[0] == '#';
        if (t[0] == '#') {                   // preprocessor line: column 0, no nesting change
            out += inStr ? raw : rtrim(t); out += eol;
            size_t k = 1;
            while (k < t.size() && (t[k] == ' ' || t[k] == '\t')) k++;
            size_t ks = k;
            while (k < t.size() && (std::isalnum((unsigned char)t[k]) || t[k] == '_')) k++;
            std::string kw = t.substr(ks, k - ks);
            // Each branch, and the code after `#endif`, starts from the state at the `#if`.
            if (kw == "if" || kw == "ifdef" || kw == "ifndef") {
                condStr.push_back(inStr);
                condLast.push_back(last);
                condDepth.push_back(depth);
                condLayout.push_back(layout);
            } else if ((kw == "elif" || kw == "else") && !condStr.empty()) {
                inStr = condStr.back();
                last = condLast.back();
                depth = condDepth.back();
                layout = condLayout.back();
            } else if (kw == "endif" && !condStr.empty()) {
                inStr = condStr.back();
                last = condLast.back();
                condStr.pop_back();
                condLast.pop_back();
                condDepth.pop_back();
                condLayout.pop_back();
            }
            if (prevCont) ppLine = t.substr(0, t.size() - 1);
            else if (!shebang) inBlock = directiveOpensComment(t);
            continue;
        }

        // A line inside open parentheses, or after one that did not end a statement
        // (with `;`, `{`, `}` or a label's `:`), continues that statement: it keeps its
        // indentation relative to the statement's first line, at least 4 more (an item
        // of a multi-line list after a `,` keeps its own).
        // A block's `{` on its own line (`int f()` then `{`) starts at its block's level.
        bool blockOpen = t[0] == '{' && (layout.kinds.empty() || layout.kinds.back() == 'B') &&
                         layout.paren == 0 && last != '=' && last != ',' && last != '(' && last != '[';
        bool contLine = t[0] != '}' && !blockOpen && (layout.paren > 0 ||
                        (last != 0 && last != ';' && last != '{' && last != '}' && last != ':'));
        bool listItem = last == ',' && layout.paren == 0;
        std::vector<std::string> pieces = fmtLayoutLine(layout, t, last);
        bool sepPiece = false;    // the previous piece of this line closed a top-level block
        for (size_t pi = 0; pi < pieces.size(); ++pi) {
            std::string piece = pieces[pi];
            if (sepPiece && piece[0] != '}') out += eol;   // as for a definition ending a line
            int ind;
            if (pi == 0 && contLine) {
                int diff = (int)a - stmtOrig;
                ind = stmtNew + (listItem ? std::max(0, diff) : std::max(4, diff));
            } else {
                // A statement's indent dedents for each leading `}`.
                int lead = depth;
                for (char c : piece) { if (c == '}') lead--; else break; }
                if (lead < 0) lead = 0;
                ind = lead * 4;
                stmtOrig = (int)a;
                stmtNew = ind;
            }
            scanNesting(piece, 0);
            if (!inStr) piece = rtrim(piece);    // trailing blanks inside an open string are its bytes
            out.append((size_t)ind, ' ');
            out += piece; out += eol;
            sepPiece = depth == 0 && piece[0] == '}';
        }
        sepNext = depth == 0 && pieces.back()[0] == '}';
    }
    return out;
}

// The wrap pass of `eskiuc fmt --width N` (selfhost fmt_wrap is the same). A code line
// longer than `width` bytes is broken after the commas of its outermost bracketed list
// (the commas at the smallest depth of `(`, `[` or `{` inside the line, at least 1): the
// pieces fill each line up to the width, and the lines after the first are indented 8
// more than it. A resulting line that is still too long is broken the same way, so
// the output is a fixed point. A line is left as written when it holds a comment, is a
// directive, or starts or ends inside a string or a block comment.
static void wrapLine(const std::string& line, int width, std::vector<std::string>& out) {
    if ((int)line.size() <= width) { out.push_back(line); return; }
    size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ') indent++;
    std::vector<std::pair<size_t, int>> commas;
    int depth = 0;
    char quote = 0;
    for (size_t i = indent; i < line.size(); ++i) {
        char c = line[i];
        if (quote) {
            if (c == '\\') i++;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') depth--;
        else if (c == ',' && depth >= 1) commas.push_back({i, depth});
    }
    int least = 0;
    for (const auto& cm : commas) if (least == 0 || cm.second < least) least = cm.second;
    std::vector<std::string> pieces;
    size_t start = 0;
    for (const auto& cm : commas) {
        if (cm.second != least) continue;
        pieces.push_back(line.substr(start, cm.first + 1 - start));
        start = cm.first + 1;
    }
    if (pieces.empty()) { out.push_back(line); return; }
    pieces.push_back(line.substr(start));
    std::string cont(indent + 8, ' ');
    std::vector<std::string> lines;
    std::string cur = pieces[0];
    for (size_t k = 1; k < pieces.size(); ++k) {
        std::string p = pieces[k];
        size_t lead = 0;
        while (lead < p.size() && p[lead] == ' ') lead++;
        p = p.substr(lead);
        if (p.empty()) continue;
        if ((int)(cur.size() + 1 + p.size()) <= width) cur += " " + p;
        else { lines.push_back(cur); cur = cont + p; }
    }
    lines.push_back(cur);
    if (lines.size() == 1) { out.push_back(line); return; }
    for (const auto& l : lines) wrapLine(l, width, out);
}

std::string wrapLongLines(const std::string& text, int width) {
    std::string out;
    bool inBlock = false;
    char quote = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        bool hasNl = nl != std::string::npos;
        std::string line = text.substr(pos, (hasNl ? nl : text.size()) - pos);
        pos = hasNl ? nl + 1 : text.size();
        bool plain = !inBlock && quote == 0;
        bool comment = false;
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            char n = i + 1 < line.size() ? line[i + 1] : 0;
            if (inBlock) {
                if (c == '*' && n == '/') { inBlock = false; i++; }
                continue;
            }
            if (quote) {
                if (c == '\\') i++;
                else if (c == quote) quote = 0;
                continue;
            }
            if (c == '/' && n == '/') { comment = true; break; }
            if (c == '/' && n == '*') { inBlock = true; comment = true; i++; continue; }
            if (c == '"' || c == '\'') quote = c;
        }
        if (quote == '\'') quote = 0;
        size_t first = line.find_first_not_of(' ');
        bool directive = first != std::string::npos && line[first] == '#';
        if (plain && !comment && !directive && !inBlock && quote == 0 && (int)line.size() > width) {
            std::vector<std::string> parts;
            wrapLine(line, width, parts);
            for (size_t k = 0; k < parts.size(); ++k) {
                out += parts[k];
                if (k + 1 < parts.size()) out += '\n';
            }
        } else {
            out += line;
        }
        if (hasNl) out += '\n';
    }
    return out;
}

// `eskiuc fmt [--check] [--width N] file.esk …` — reformat each file in place. With
// --check, don't write; exit non-zero if any file is not already formatted. With
// --width, also break code lines longer than N (wrapLongLines). Returns the process
// exit code.
int runFmt(const std::vector<std::string>& files, bool check, int width) {
    if (files.empty()) { std::cerr << "error: fmt: no input files\n"; return 1; }
    int changed = 0, failed = 0;
    for (const auto& f : files) {
        std::ifstream in(f);
        if (!in.is_open()) { std::cerr << "error: fmt: cannot open '" << f << "'\n"; failed++; continue; }
        std::stringstream buf; buf << in.rdbuf(); in.close();
        std::string original = buf.str();
        std::string formatted = formatSource(original);
        if (width > 0) formatted = wrapLongLines(formatted, width);
        if (formatted == original) continue;
        changed++;
        if (check) { std::cout << f << "\n"; continue; }
        std::ofstream outF(f, std::ios::trunc);
        if (!outF.is_open()) { std::cerr << "error: fmt: cannot write '" << f << "'\n"; failed++; continue; }
        outF << formatted; outF.close();
    }
    if (failed) return 1;
    if (check && changed) return 1;     // CI signal: files need formatting
    return 0;
}

void seedPredefinedMacros(std::map<std::string, Macro>& macros, const std::string& triple,
                          bool freestanding) {
    // Predefine a platform macro so stdlib can #ifdef per OS (the event-loop
    // backend and sockaddr_in layout differ between macOS and Linux). It follows
    // the --target triple when cross-compiling, else the build host — otherwise a
    // `--target x86_64-linux-gnu` build on macOS would still select the kqueue
    // path and emit unresolved BSD symbols.
    Macro os; os.body = "1";
    const std::string& tt = triple;
    bool tgtLinux = tt.find("linux") != std::string::npos;
    bool tgtApple = tt.find("apple") != std::string::npos ||
                    tt.find("darwin") != std::string::npos ||
                    tt.find("macos") != std::string::npos;
    bool tgtWindows = tt.find("windows") != std::string::npos ||
                      tt.find("win32") != std::string::npos ||
                      tt.find("mingw") != std::string::npos;
    // _WIN64 accompanies _WIN32 on 64-bit Windows (MSVC keeps _WIN32 defined
    // for both widths and adds _WIN64 only when 64-bit).
    bool tgt64 = tt.find("x86_64") != std::string::npos ||
                 tt.find("amd64") != std::string::npos ||
                 tt.find("aarch64") != std::string::npos ||
                 tt.find("arm64") != std::string::npos;
    if (tgtLinux)        { macros["__linux__"] = os; }
    else if (tgtApple)   { macros["__APPLE__"] = os; }
    else if (tgtWindows) {
        macros["_WIN32"] = os;
        if (tgt64) macros["_WIN64"] = os;
    }
    else if (tt.empty()) {
        // Native build: follow the build host.
#if defined(_WIN32)
        macros["_WIN32"] = os;
#if defined(_WIN64)
        macros["_WIN64"] = os;
#endif
#elif defined(__APPLE__)
        macros["__APPLE__"] = os;
#elif defined(__linux__)
        macros["__linux__"] = os;
#endif
    }
    // else: an explicit bare-metal or otherwise non-hosted triple (e.g. the
    // 3DS's armv6k-none-eabihf) defines no OS macro. Bare metal has no host OS,
    // so portable code guards that path explicitly rather than falling through
    // to the build host's.

    // The architecture macro C compilers predefine (__aarch64__ / __x86_64__ /
    // __arm__), from the --target triple or else the build host. The self-hosted
    // compiler reads it at its own build to pick its C-ABI lowering.
    std::string arch;
    if (tt.rfind("aarch64", 0) == 0 || tt.rfind("arm64", 0) == 0) arch = "__aarch64__";
    else if (tt.rfind("x86_64", 0) == 0 || tt.rfind("amd64", 0) == 0) arch = "__x86_64__";
    else if (tt.rfind("arm", 0) == 0 || tt.rfind("thumb", 0) == 0) arch = "__arm__";
    else if (tt.empty()) {
#if defined(__aarch64__) || defined(_M_ARM64)
        arch = "__aarch64__";
#elif defined(__x86_64__) || defined(_M_X64)
        arch = "__x86_64__";
#elif defined(__arm__)
        arch = "__arm__";
#endif
    }
    if (!arch.empty()) macros[arch] = os;

    // WebAssembly, Android and iOS, spelled as clang predefines them.
    if (tt.rfind("wasm32", 0) == 0) {
        macros["__wasm__"] = os;
        macros["__wasm32__"] = os;
        if (tt.find("emscripten") != std::string::npos) macros["__EMSCRIPTEN__"] = os;
        if (tt.find("wasi") != std::string::npos) macros["__wasi__"] = os;
    }
    if (tt.find("android") != std::string::npos) macros["__ANDROID__"] = os;
    if (tt.find("-ios") != std::string::npos) {
        macros["TARGET_OS_IPHONE"] = os;
        macros["TARGET_OS_IOS"] = os;
        if (tt.find("simulator") != std::string::npos) macros["TARGET_IPHONE_SIMULATOR"] = os;
    }

    // Predefine __ESKIU_FREESTANDING__ under --freestanding so stdlib (e.g.
    // <mem>'s alloc/free) can target esk_alloc/esk_free instead of libc.
    if (freestanding) {
        Macro fs; fs.body = "1";
        macros["__ESKIU_FREESTANDING__"] = fs;
    }
}

// Load → lex → parse every input file and merge their declarations into one Program,
// or nullptr on a lexical or parse error (a diagnostic is printed by the lexer or
// parser; `lexFailed` tells which). Shared by the build and every pipeline mode
// (parse/typecheck/codegen, --hover-at, --definition-at), so they all preprocess alike
// (same predefined macros, one macro table across files and imports, each import
// preprocessed at its import line, __FILE__ = the path) and see the same program.
// `macrosOut` receives the final macro table.
std::shared_ptr<Program> loadProgram(const std::vector<std::string>& inputs, const std::string& triple,
                                     bool freestanding, std::map<std::string, Macro>* macrosOut,
                                     bool* lexFailed, const std::string* primarySource) {
    std::map<std::string, Macro> macros;     // shared: #defines propagate across files
    seedPredefinedMacros(macros, triple, freestanding);
    std::set<std::string> importedFiles;     // shared: a common import is parsed once
    ImportCache cache;                       // imports preprocessed at their import line
    cache.stdlibPath = stdlibRoot;
    cache.macros = &macros;
    std::vector<DeclPtr> merged;
    std::vector<std::string> linkLibs;       // `#pragma link` libraries of every input
    if (lexFailed) *lexFailed = false;
    if (!inputs.empty()) pkgLoadRoots(inputs[0], triple);   // package imports from eskiu.lock
    for (const auto& fname : inputs) {
        // Register the root file itself, so an import cycle back to it (or an
        // input that an earlier input already imported) is not parsed twice.
        std::string canon = Parser::canonicalPath(fname);
        if (!importedFiles.insert(canon).second) continue;
        cache.seen.insert(canon);
        // The first input may be given as text (editor tooling: an unsaved buffer).
        std::string source = primarySource && fname == inputs.front() ? *primarySource : readFile(fname);
        PPImportHook hook = cache.hookFor(dirOf(fname));
        Lexer lexer(source, &macros, fname, &hook);
        std::vector<Token> tokens;
        Token tok = lexer.next_token();
        while (tok.type != TokenType::EOF_TOKEN) {
            tokens.push_back(tok);
            tok = lexer.next_token();
        }
        tokens.push_back(tok);
        if (lexer.hadError) { if (lexFailed) *lexFailed = true; return nullptr; }

        Parser parser(tokens);
        parser.sourceText = &lexer.text();
        parser.filename = fname;
        parser.stdlibPath = stdlibRoot;
        parser.basedir = dirOf(fname);
        parser.macros = &macros;
        parser.importedFiles = &importedFiles;
        parser.importCache = &cache;
        auto prog = parser.parse();
        if (!prog) return nullptr;
        merged.insert(merged.end(), prog->declarations.begin(), prog->declarations.end());
        linkLibs.insert(linkLibs.end(), prog->linkLibs.begin(), prog->linkLibs.end());
    }
    // A program has one main: a script's statements are its main, so no other input may
    // define one or have top-level statements too (one file is checked by its parser).
    const FunctionDecl* scriptMain = nullptr;
    const FunctionDecl* namedMain = nullptr;
    for (const auto& d : merged) {
        auto* f = dynamic_cast<FunctionDecl*>(d.get());
        if (!f || f->name != "main" || f->fromImport) continue;
        bool script = f->params.size() == 2 && f->params[0].second == "__argc" && f->body;
        if (!script) { if (!namedMain) namedMain = f; continue; }
        if (scriptMain) {
            std::cerr << "error: " << f->sourceFile << ":" << f->body->line << ":" << f->body->col
                      << ": only one input file may have top-level statements (they are its main)\n";
            return nullptr;
        }
        scriptMain = f;
    }
    if (scriptMain && namedMain) {
        std::cerr << "error: " << scriptMain->sourceFile << ":" << scriptMain->body->line << ":"
                  << scriptMain->body->col << ": a file with top-level statements has no 'main': the statements are its main\n";
        return nullptr;
    }
    if (macrosOut) *macrosOut = macros;
    auto program = std::make_shared<Program>(merged);
    program->linkLibs = linkLibs;
    return program;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The Android NDK: $ANDROID_NDK_HOME, $ANDROID_NDK_ROOT, else the newest version under
// the SDK's ndk/ directory ($ANDROID_HOME, $ANDROID_SDK_ROOT, or the SDK manager's
// default place in the home directory).
std::string androidNdk() {
    for (const char* v : {"ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"})
        if (const char* p = std::getenv(v); p && *p) return p;
    std::vector<std::string> sdks;
    for (const char* v : {"ANDROID_HOME", "ANDROID_SDK_ROOT"})
        if (const char* p = std::getenv(v); p && *p) sdks.push_back(p);
    if (const char* home = std::getenv("HOME"); home && *home) {
        sdks.push_back(std::string(home) + "/Library/Android/sdk");
        sdks.push_back(std::string(home) + "/Android/Sdk");
    }
    for (const auto& sdk : sdks) {
        std::error_code ec;
        std::string best;
        for (auto it = std::filesystem::directory_iterator(sdk + "/ndk", ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            std::string name = it->path().filename().string();
            auto key = [](const std::string& s) {
                std::vector<long> parts;
                std::stringstream ss(s);
                for (std::string p; std::getline(ss, p, '.');) parts.push_back(std::atol(p.c_str()));
                return parts;
            };
            if (best.empty() || key(name) > key(best)) best = name;
        }
        if (!best.empty()) return sdk + "/ndk/" + best;
    }
    return "";
}

// The C driver a --target build for WebAssembly, Android or iOS uses when $CC is not
// set, with the arguments that select the target: emcc for emscripten, clang with
// --target for the other wasm32 triples (the LLVM toolchain's own, which has the
// WebAssembly backend and wasm-ld), the NDK's clang for Android (from
// $ANDROID_NDK_HOME or $ANDROID_NDK_ROOT) and `xcrun -sdk iphoneos clang` for iOS.
static bool crossDriver(const std::string& triple, std::string& driver, std::vector<std::string>& args) {
    auto has = [&](const char* s) { return triple.find(s) != std::string::npos; };
    if (triple.rfind("wasm32", 0) == 0 && has("emscripten")) {
        if (auto p = llvm::sys::findProgramByName("emcc")) { driver = *p; return true; }
        return false;
    }
    if (triple.rfind("wasm32", 0) == 0) {
#ifdef ESKIU_LLVM_BINDIR
        std::string llvmClang = std::string(ESKIU_LLVM_BINDIR) + "/clang";
        if (llvm::sys::fs::exists(llvmClang)) driver = llvmClang;
#endif
        if (driver.empty())
            if (auto p = llvm::sys::findProgramByName("clang")) driver = *p;
        if (driver.empty()) return false;
        args = {"--target=" + triple};
        return true;
    }
    if (has("android")) {
        std::string ndk = androidNdk();
        if (ndk.empty()) return false;
        for (const char* host : {"darwin-x86_64", "linux-x86_64", "windows-x86_64"}) {
            std::string c = ndk + "/toolchains/llvm/prebuilt/" + host + "/bin/clang";
            if (llvm::sys::fs::exists(c) || llvm::sys::fs::exists(c + ".exe")) {
                driver = llvm::sys::fs::exists(c) ? c : c + ".exe";
                args = {"--target=" + triple};
                return true;
            }
        }
        return false;
    }
    if (has("-ios")) {
        auto p = llvm::sys::findProgramByName("xcrun");
        if (!p) return false;
        driver = *p;
        args = {"-sdk", has("simulator") ? "iphonesimulator" : "iphoneos", "clang", "-target", triple};
        return true;
    }
    return false;
}

// Locate a C linker driver: $CC first, then cc / clang / gcc on PATH.
// When `sanitized` is set, prefer the clang from the LLVM toolchain this
// compiler was built against — its compiler-rt matches the ASan instrumentation
// we emit, so the runtime versions agree (Apple's system clang ships a different
// ASan ABI and would fail to link).
std::string findCDriver(bool sanitized, std::vector<std::string>* ccArgs, const std::string& triple) {
#ifdef ESKIU_LLVM_BINDIR
    if (sanitized) {
        std::string llvmClang = std::string(ESKIU_LLVM_BINDIR) + "/clang";
        if (llvm::sys::fs::exists(llvmClang)) return llvmClang;
    }
#endif
    if (const char* cc = std::getenv("CC"); cc && *cc) {
        // $CC may carry arguments ("clang -m64"): the first word is the program.
        std::vector<std::string> words;
        std::istringstream ws(cc);
        for (std::string w; ws >> w;) words.push_back(w);
        if (!words.empty()) {
            if (auto p = llvm::sys::findProgramByName(words[0])) {
                if (ccArgs) ccArgs->assign(words.begin() + 1, words.end());
                return *p;
            }
            std::cerr << "warning: $CC ('" << cc << "') was not found; "
                         "falling back to cc/clang/gcc" << std::endl;
        }
    }
    if (!triple.empty()) {
        std::string driver;
        std::vector<std::string> args;
        if (crossDriver(triple, driver, args)) {
            if (ccArgs) *ccArgs = args;
            return driver;
        }
    }
    for (const char* name : {"cc", "clang", "gcc"}) {
        if (auto p = llvm::sys::findProgramByName(name)) return *p;
    }
    return "";
}

// Link an object file into an executable by invoking the system C driver
// (the same thing rustc/clang do internally). Returns true on success.
bool linkExecutable(const std::string& obj, const std::string& out,
                           const std::vector<std::string>& libs,
                           const std::vector<std::string>& paths,
                           const std::vector<std::string>& extra,
                           bool sanitized, const std::string& triple) {
    std::vector<std::string> ccArgs;
    std::string driver = findCDriver(sanitized, &ccArgs, triple);
    if (driver.empty()) {
        std::cerr << "error: no C linker driver found (looked for $CC, cc, clang, gcc).\n"
                     "       Install a C toolchain, or use -c to emit an object file "
                     "and link it yourself.\n";
        return false;
    }
    std::vector<std::string> argv = {driver};
    argv.insert(argv.end(), ccArgs.begin(), ccArgs.end());
    argv.insert(argv.end(), {obj, "-o", out});
    // A shared library (.so, .dylib, .dll): the object is position-independent already.
    if (endsWith(out, ".so") || endsWith(out, ".dylib") || endsWith(out, ".dll")) argv.push_back("-shared");
    for (const auto& p : paths) argv.push_back("-L" + p);
    for (const auto& l : libs)  argv.push_back("-l" + l);
    for (const auto& a : extra) argv.push_back(a);

    std::vector<llvm::StringRef> args(argv.begin(), argv.end());
    std::string errMsg;
    int rc = llvm::sys::ExecuteAndWait(driver, args, /*Env=*/std::nullopt,
                                       /*Redirects=*/{}, /*SecondsToWait=*/0,
                                       /*MemoryLimit=*/0, &errMsg);
    if (rc != 0) {
        std::cerr << "error: linking failed";
        if (!errMsg.empty()) std::cerr << ": " << errMsg;
        else                 std::cerr << " (" << driver << " exited with code " << rc << ")";
        std::cerr << std::endl;
        return false;
    }
    return true;
}

bool writeStaticLibrary(const std::string& obj, const std::string& out, const std::string& tripleStr) {
    llvm::Triple t(tripleStr.empty() ? llvm::sys::getDefaultTargetTriple() : tripleStr);
    auto member = llvm::NewArchiveMember::getFile(obj, /*Deterministic=*/true);
    if (!member) {
        std::cerr << "error: " << llvm::toString(member.takeError()) << std::endl;
        return false;
    }
    // The member is named after the library, not the temporary object.
    static llvm::BumpPtrAllocator alloc;
    static llvm::StringSaver saver(alloc);
    std::string base = llvm::sys::path::stem(out).str();
    member->MemberName = saver.save(base + (t.isOSWindows() ? ".obj" : ".o"));
    llvm::object::Archive::Kind kind = t.isOSDarwin() ? llvm::object::Archive::K_DARWIN
                                     : t.isOSWindows() ? llvm::object::Archive::K_COFF
                                     : llvm::object::Archive::K_GNU;
    std::vector<llvm::NewArchiveMember> members;
    members.push_back(std::move(*member));
    if (llvm::Error e = llvm::writeArchive(out, members, llvm::SymtabWritingMode::NormalSymtab,
                                           kind, /*Deterministic=*/true, /*Thin=*/false)) {
        std::cerr << "error: cannot write " << out << ": " << llvm::toString(std::move(e)) << std::endl;
        return false;
    }
    return true;
}

std::vector<std::string> implicitLinkLibs(const std::map<std::string, Macro>& macros,
                                          bool usesExceptions, bool usesThreads) {
    std::vector<std::string> libs;
    bool apple = macros.count("__APPLE__") > 0;
    bool android = macros.count("__ANDROID__") > 0;
    bool gnu = !android && (macros.count("__linux__") > 0 || macros.count("_WIN32") > 0);
    // Android: the NDK's static libc++ and its ABI library; pthread is in bionic's libc.
    if (usesExceptions) {
        if (apple) libs.push_back("c++");
        else if (android) { libs.push_back("c++_static"); libs.push_back("c++abi"); }
        else if (gnu) libs.push_back("stdc++");
    }
    if (usesThreads && gnu) libs.push_back("pthread");
    return libs;
}

// Run an executable, forwarding `progArgs`, and return its exit code: the program's
// own status, 128+N when it was killed by signal N (the shell convention), or 1 if it
// could not be launched. Used by `eskiuc run`.
int runExecutable(const std::string& exe, const std::vector<std::string>& progArgs) {
#ifndef _WIN32
    std::vector<char*> cargv;
    cargv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& a : progArgs) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "error: could not run '" << exe << "': fork failed" << std::endl;
        return 1;
    }
    if (pid == 0) {
        execv(exe.c_str(), cargv.data());
        std::cerr << "error: could not run '" << exe << "'" << std::endl;
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) { std::cerr << "error: waiting for '" << exe << "' failed" << std::endl; return 1; }
    }
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
#else
    std::vector<std::string> argv = {exe};
    for (const auto& a : progArgs) argv.push_back(a);
    std::vector<llvm::StringRef> args(argv.begin(), argv.end());
    std::string errMsg;
    int rc = llvm::sys::ExecuteAndWait(exe, args, /*Env=*/std::nullopt,
                                       /*Redirects=*/{}, /*SecondsToWait=*/0,
                                       /*MemoryLimit=*/0, &errMsg);
    if (rc < 0) {
        std::cerr << "error: could not run '" << exe << "'";
        if (!errMsg.empty()) std::cerr << ": " << errMsg;
        std::cerr << std::endl;
        return 1;
    }
    return rc;
#endif
}
