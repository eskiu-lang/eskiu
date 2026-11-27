// parse_notes.cpp: the module a file belongs to, and the note after a syntax error at
// text a macro produced (split from parser.cpp).
#include "parser.h"
#include "parser_internal.h"
#include "../lexer/preprocessor.h"

std::string moduleLabelOf(const std::string& file, const std::string& stdlibRoot) {
    auto under = [&](const std::string& dir) {
        return file.size() > dir.size() + 1 && file.compare(0, dir.size(), dir) == 0 && file[dir.size()] == '/';
    };
    for (const auto& [name, root] : packageRoots.roots)
        if (!root.empty() && under(root)) return "package '" + name + "'";
    std::string lib = stdlibRoot.empty() ? "stdlib" : stdlibRoot + "/stdlib";
    if (under(lib) && file.size() > lib.size() + 5 && file.compare(file.size() - 4, 4, ".esk") == 0)
        return "<" + file.substr(lib.size() + 1, file.size() - lib.size() - 5) + ">";
    return "";
}

// The note after a parse error at a token a macro produced: which macro, from where.
std::string macroNote(const std::string& file, const PPExpansion& x, const std::string& stdlibRoot) {
    std::string text = x.text.size() > 40 ? x.text.substr(0, 37) + "..." : x.text;
    std::string what = "'" + x.name + "' is a predefined macro";
    if (!x.file.empty()) {
        std::string from = moduleLabelOf(x.file, stdlibRoot);
        what = "'" + x.name + "' is a macro (" + (from.empty() ? "" : "from " + from + ", ") + "defined at " +
               x.file + ":" + std::to_string(x.defLine) + ")";
    }
    return "note: " + file + ":" + std::to_string(x.line) + ":" + std::to_string(x.srcCol) + ": " + what +
           " and expands to '" + text + "' here; rename yours or #undef it";
}
