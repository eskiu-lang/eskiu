#include "lexer.h"
#include <cstdint>
#include <iostream>
#include <cctype>
#include <sstream>
#include <map>
#include <vector>
#include <set>
#include <algorithm>
#include "preprocessor.h"

// ── Preprocessor ────────────────────────────────────────────────────────────
// Text pass run before lexing: object-like and function-like #define/#undef,
// and #ifdef/#ifndef/#else/#endif conditionals. Directive and skipped lines
// become blank lines so source line numbers are preserved. The macro table is
// supplied by the caller and shared across files, so #defines propagate through
// import / multi-file compilation; an import is preprocessed at its import line
// (see PPImportHook), in C's textual order. Substitution is identifier-aware (skips
// string/char literals and line comments) and recursive (a macro is not
// re-expanded within its own expansion). A function-like macro call may span lines,
// and a string literal may too: a line that starts inside one is string text up to
// its closing quote (never a directive).

#include "pp_expand.h"
#include "pp_cond.h"

void preprocess(const std::string& src,
                       std::map<std::string, Macro>& defines,
                       std::string& result,
                       const std::string& filename,
                       bool& hadErr,
                       const PPImportHook* importHook) {
    // Predefined `__FILE__` (constant for this file). `__LINE__` is refreshed each
    // line below. Both are ordinary object-like macros so ppExpand handles them
    // with correct identifier boundaries.
    Macro fileMacro; fileMacro.body = ppFileLiteral(filename);
    defines["__FILE__"] = fileMacro;
    const std::string fileLabel = filename.empty() ? "<input>" : filename;
    auto ppError = [&](int ln, int col, const std::string& msg) {
        std::cerr << "error: " << fileLabel << ":" << ln << ":" << col << ": " << msg << std::endl;
        hadErr = true;
    };
    // One entry per open conditional. `anyTaken`: some branch of this #if chain
    // has already been selected (so a later #elif/#else is skipped).
    struct Cond { bool parentActive; bool branchActive; bool anyTaken; bool sawElse; int line; int col; };
    std::vector<Cond> stack;
    auto active = [&]() {
        return stack.empty() ? true : (stack.back().parentActive && stack.back().branchActive);
    };

    // A leading UTF-8 byte order mark is not part of the program.
    std::istringstream in(src.compare(0, 3, "\xEF\xBB\xBF") == 0 ? src.substr(3) : src);
    std::ostringstream out;
    std::string line; bool first = true;
    int curLine = 0;
    bool inBlockComment = false;     // a /* */ comment is open at the start of this line
    // A string literal is open at the start of this line (the lexer's strings may span
    // lines): the line is string text up to the closing quote, never a directive.
    bool inString = false;
    // CRLF input: getline leaves the '\r', which would hide a trailing '\'
    // continuation and leak into directive operands. Drop it up front.
    auto stripCR = [](std::string& l) { if (!l.empty() && l.back() == '\r') l.pop_back(); };
    while (std::getline(in, line)) {
        stripCR(line);
        curLine++;                       // physical line of this logical line
        int lineNo = curLine;            // __LINE__ for this logical line
        // Line splicing: a trailing backslash continues onto the next physical
        // line, so a #define (or any line) may span several lines. The joined
        // logical line is emitted as one line followed by `extra` blank lines,
        // keeping every later source line on its original line number.
        int extra = 0;
        size_t dh = line.find_first_not_of(" \t");
        bool directive = !inBlockComment && !inString && dh != std::string::npos && line[dh] == '#';
        while (!line.empty() && line.back() == '\\' && backslashContinuesLine(line, directive, inString)) {
            line.pop_back();
            std::string cont;
            if (!std::getline(in, cont)) break;
            stripCR(cont);
            line += cont;
            extra++;
            curLine++;                   // each continuation is a physical line too
        }

        if (!first) out << "\n";
        first = false;

        size_t h = line.find_first_not_of(" \t");
        bool handled = false;
        // A `#!` first line is a shebang (`#!/usr/bin/env -S eskiuc run`), not a directive.
        bool shebang = lineNo == 1 && line.compare(0, 2, "#!") == 0;
        bool dirOpen = false;            // the directive line leaves a /* comment open
        if (!inBlockComment && !inString && h != std::string::npos && line[h] == '#') {
            handled = true;
            if (!shebang) line = ppStripComments(line, dirOpen);
            int col = (int)h + 1;
            size_t kp = h + 1;
            while (kp < line.size() && (line[kp] == ' ' || line[kp] == '\t')) kp++;
            size_t ks = kp;
            while (kp < line.size() && (std::isalnum((unsigned char)line[kp]) || line[kp] == '_')) kp++;
            std::string kw = line.substr(ks, kp - ks);
            std::string rest = ppTrim(line.substr(kp));
            std::string operand = rest.substr(0, rest.find_first_of(" \t"));
            if (kw == "define") {
                if (active()) {
                    size_t p = 0;
                    while (p < rest.size() && (std::isalnum((unsigned char)rest[p]) || rest[p]=='_')) p++;
                    std::string name = rest.substr(0, p);
                    Macro mac;
                    if (p < rest.size() && rest[p] == '(') {        // function-like
                        mac.isFunction = true;
                        size_t q = p + 1; std::string cur;
                        for (; q < rest.size(); ++q) {
                            char d = rest[q];
                            if (d == ')') { q++; break; }
                            if (d == ',') { std::string t = ppTrim(cur); if (!t.empty()) mac.params.push_back(t); cur.clear(); }
                            else cur += d;
                        }
                        std::string t = ppTrim(cur); if (!t.empty()) mac.params.push_back(t);
                        mac.body = ppTrim(q < rest.size() ? rest.substr(q) : "");
                    } else {
                        mac.body = ppTrim(p < rest.size() ? rest.substr(p) : "");
                    }
                    if (name.empty()) ppError(lineNo, col, "expected a macro name after #define");
                    else if (ppBodyHasHash(mac.body))
                        ppError(lineNo, col, "macro '" + name + "': '#' stringification and '##' token pasting are not supported");
                    else defines[name] = mac;
                }
            } else if (kw == "undef") {
                if (active()) defines.erase(operand);
            } else if (kw == "ifdef" || kw == "ifndef") {
                bool on = false;
                if (active()) {
                    if (operand.empty()) ppError(lineNo, col, "expected a macro name after #" + kw);
                    on = (defines.count(operand) > 0) == (kw == "ifdef");
                }
                stack.push_back({active(), on, on, false, lineNo, col});
            } else if (kw == "if") {
                bool on = false;
                if (active()) {
                    long long v = 0; std::string err;
                    if (ppEvalIf(rest, defines, v, err)) on = v != 0;
                    else ppError(lineNo, col, err);
                }
                stack.push_back({active(), on, on, false, lineNo, col});
            } else if (kw == "elif") {
                if (stack.empty()) ppError(lineNo, col, "#elif without #if");
                else if (stack.back().sawElse) ppError(lineNo, col, "#elif after #else");
                else {
                    Cond& c = stack.back();
                    bool on = false;
                    if (c.parentActive && !c.anyTaken) {
                        long long v = 0; std::string err;
                        if (ppEvalIf(rest, defines, v, err)) on = v != 0;
                        else ppError(lineNo, col, err);
                    }
                    c.branchActive = on;
                    c.anyTaken = c.anyTaken || on;
                }
            } else if (kw == "else") {
                if (stack.empty()) ppError(lineNo, col, "#else without #if");
                else if (stack.back().sawElse) ppError(lineNo, col, "#else after #else");
                else {
                    Cond& c = stack.back();
                    c.branchActive = !c.anyTaken;
                    c.anyTaken = true;
                    c.sawElse = true;
                }
            } else if (kw == "endif") {
                if (stack.empty()) ppError(lineNo, col, "#endif without #if");
                else stack.pop_back();
            } else if (kw == "pragma") {
                // #pragma is a compiler directive, not a preprocessor one: pass
                // it through unchanged so the lexer/parser can act on it (e.g.
                // `#pragma pack`). Unknown pragmas are ignored downstream.
                if (active()) out << ppTrim(line.substr(h));
            } else if (kw == "error") {
                // #error <message> — abort compilation with the message (only on
                // an active branch, so it can guard #ifdef blocks).
                if (active()) ppError(lineNo, col, "#error " + rest);
            } else if (kw == "include") {
                if (active())
                    ppError(lineNo, col, "#include is not supported; use `import \"file.esk\";` "
                                         "or `import <module>;` instead");
            } else if (kw.empty() && (rest.empty() || shebang)) {
                // `#` alone is the null directive; a `#!` first line is a shebang.
            } else if (active()) {
                ppError(lineNo, col, "unknown preprocessor directive '#" + (kw.empty() ? rest : kw) + "'");
            }
        }

        if (!handled && active()) {
            // The text of a string open at the start of the line is kept as is.
            std::string lead;
            bool strOpen = false;
            if (inString) {
                size_t e = ppStringClose(line);
                strOpen = e == std::string::npos;
                if (strOpen) e = line.size();
                lead = line.substr(0, e);
                line = line.substr(e);
            }
            { Macro m; m.body = std::to_string(lineNo); defines["__LINE__"] = m; }
            std::set<std::string> expanding;
            PPExpandCtx ctx;
            bool blkStart = inBlockComment;
            std::string expanded;
            // A function-like macro call whose argument list continues on the
            // following lines: join them (newline kept, read as a space inside the
            // arguments) and expand again. The joined lines become trailing blanks.
            for (int joins = 0;; joins++) {
                bool blk = blkStart;
                expanding.clear();
                ctx = PPExpandCtx();
                expanded = ppExpand(line, defines, expanding, &blk, &ctx);
                std::string cont;
                bool join = ctx.openCall || (ctx.trailingFnName && ppNextCodeIsParen(in));
                if (join && joins < kMaxMacroJoin && std::getline(in, cont)) {
                    stripCR(cont);
                    line += "\n" + cont;
                    extra++;
                    curLine++;
                    continue;
                }
                inBlockComment = blk;
                break;
            }
            inString = strOpen || ppEndsInString(line, blkStart);
            if (!ctx.err.empty()) ppError(lineNo, (int)(ctx.errCol + lead.size()), ctx.err);
            out << lead << expanded;
            extra -= (int)std::count(expanded.begin(), expanded.end(), '\n');
            if (importHook) {
                ppScanImports(expanded, blkStart, *importHook);
                defines["__FILE__"] = fileMacro;
            }
        } else {
            // Inactive and directive lines emit blank. When one opens or closes a
            // `/* */` comment, emit just the delimiter so the lexer's view of the
            // comment matches the source (a directive's `/* ...` continuing onto
            // the next lines must not leave a stray `*/` behind).
            bool was = inBlockComment;
            inBlockComment = handled ? dirOpen : endsInBlockComment(line, inBlockComment);
            if (!was && inBlockComment) out << "/*";
            else if (was && !inBlockComment) out << "*/";
        }

        for (int e = 0; e < extra; ++e) out << "\n";  // preserve line numbers
    }
    for (const Cond& c : stack)
        ppError(c.line, c.col, "unterminated conditional directive (missing #endif)");
    result = out.str();
}
