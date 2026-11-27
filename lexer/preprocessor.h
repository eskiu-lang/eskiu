#pragma once

// The text preprocessing pass run before lexing: object-/function-like
// #define/#undef and #ifdef/#ifndef/#else/#endif. Split out of lexer.cpp;
// the lexer calls preprocess() before tokenizing. See preprocessor.cpp.

#include <map>
#include <string>
#include <vector>
#include "lexer.h"   // Macro

// A macro use the preprocessor replaced: where its replacement sits in the expanded text
// (line, col, len), the use's column in the source (srcCol), and where the macro was
// #defined (`file`, `defLine`; empty file: predefined). Kept per file, so a parse error at
// a token a macro produced can name the macro.
struct PPExpansion {
    int line = 0, col = 0, len = 0, srcCol = 0, defLine = 0;
    std::string name, text, file;
};
// The expansion of `file` that produced the text at line:col, or null.
const PPExpansion* ppExpansionAt(const std::string& file, int line, int col);

void preprocess(const std::string& src,
                std::map<std::string, Macro>& defines,
                std::string& result,
                const std::string& filename,
                bool& hadErr,
                const PPImportHook* importHook = nullptr);
