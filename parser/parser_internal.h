#pragma once

// Internal helpers shared across the parser's split translation units
// (parser.cpp + parse_{decl,stmt,expr}.cpp). Not part of the public Parser API.

#include <memory>
#include <string>
#include "../lexer/lexer.h"

struct PPExpansion;
// The note after a syntax error in `file` at text the macro use `x` produced (parse_notes.cpp).
std::string macroNote(const std::string& file, const PPExpansion& x, const std::string& stdlibRoot);

// Stamp any AST node with position from tok.
template<typename T>
std::shared_ptr<T> withPos(std::shared_ptr<T> node, const Token& tok) {
    node->line = tok.line; node->col = tok.column; return node;
}

// True if `t` is a primitive type keyword (the int/uint family, float/double, bool,
// char, string, void). The one list of primitive type tokens; context-specific extras
// (STAR, IDENT, QUESTION, ...) stay at each call site.
inline bool isPrimitiveTypeToken(TokenType t) {
    switch (t) {
        case TokenType::INT:  case TokenType::INT8:  case TokenType::INT16:
        case TokenType::INT32: case TokenType::INT64:
        case TokenType::UINT: case TokenType::UINT8: case TokenType::UINT16:
        case TokenType::UINT32: case TokenType::UINT64:
        case TokenType::FLOAT: case TokenType::DOUBLE:
        case TokenType::BOOL: case TokenType::CHAR:
        case TokenType::STRING: case TokenType::VOID:
            return true;
        default:
            return false;
    }
}
