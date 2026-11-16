// fmt.cpp: eskiuc fmt: formatSource, the --width wrap pass and runFmt (split from main_support.cpp).
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
#include "fmt_layout.h"

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
