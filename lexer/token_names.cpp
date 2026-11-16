// token_names.cpp: tokenTypeToString, the token kind names (split from preprocessor.cpp).
#include "lexer.h"

std::string tokenTypeToString(TokenType type) {
    switch (type) {
        // Keywords
        case TokenType::LET: return "LET";
        case TokenType::INT: return "INT";
        case TokenType::FLOAT: return "FLOAT";
        case TokenType::DOUBLE: return "DOUBLE";
        case TokenType::BOOL: return "BOOL";
        case TokenType::CHAR: return "CHAR";
        case TokenType::STRING: return "STRING";
        case TokenType::VOID: return "VOID";
        case TokenType::STRUCT: return "STRUCT";
        case TokenType::PACKED: return "PACKED";
        case TokenType::UNION:  return "UNION";
        case TokenType::INTERFACE: return "INTERFACE";
        case TokenType::ENUM: return "ENUM";
        case TokenType::FN: return "FN";
        case TokenType::OPERATOR: return "OPERATOR";
        case TokenType::ASM: return "ASM";
        case TokenType::VOLATILE:      return "VOLATILE";
        case TokenType::STATIC:        return "STATIC";
        case TokenType::ESCAPING:      return "ESCAPING";
        case TokenType::MUST_USE:      return "MUST_USE";
        case TokenType::ASYNC:         return "ASYNC";
        case TokenType::AWAIT:         return "AWAIT";
        case TokenType::CONST:         return "CONST";
        case TokenType::THREAD_CREATE: return "THREAD_CREATE";
        case TokenType::THREAD_JOIN:   return "THREAD_JOIN";
        case TokenType::FOR: return "FOR";
        case TokenType::IN: return "IN";
        case TokenType::WHILE: return "WHILE";
        case TokenType::DO: return "DO";
        case TokenType::IF: return "IF";
        case TokenType::ELSE: return "ELSE";
        case TokenType::SWITCH: return "SWITCH";
        case TokenType::MATCH: return "MATCH";
        case TokenType::CASE: return "CASE";
        case TokenType::DEFAULT: return "DEFAULT";
        case TokenType::BREAK: return "BREAK";
        case TokenType::RETURN: return "RETURN";
        case TokenType::IMPORT: return "IMPORT";
        case TokenType::EXTERN: return "EXTERN";
        case TokenType::INTRINSIC: return "INTRINSIC";
        case TokenType::ALLOC_WITH: return "ALLOC_WITH";
        case TokenType::NULL_KW: return "NULL";
        case TokenType::TRUE: return "TRUE";
        case TokenType::FALSE: return "FALSE";
        case TokenType::SIZEOF: return "SIZEOF";
        case TokenType::FREE_CLOSURE: return "FREE_CLOSURE";
        case TokenType::TRY: return "TRY";
        case TokenType::CATCH: return "CATCH";
        case TokenType::FINALLY: return "FINALLY";
        case TokenType::THROW: return "THROW";
        case TokenType::DEFER: return "DEFER";
        case TokenType::ERRDEFER: return "ERRDEFER";
        case TokenType::CONTINUE: return "CONTINUE";
        case TokenType::INT8: return "INT8";
        case TokenType::INT16: return "INT16";
        case TokenType::INT32: return "INT32";
        case TokenType::INT64: return "INT64";
        case TokenType::UINT: return "UINT";
        case TokenType::UINT8: return "UINT8";
        case TokenType::UINT16: return "UINT16";
        case TokenType::UINT32: return "UINT32";
        case TokenType::UINT64: return "UINT64";
        // Operators
        case TokenType::PLUS: return "PLUS";
        case TokenType::MINUS: return "MINUS";
        case TokenType::STAR: return "STAR";
        case TokenType::SLASH: return "SLASH";
        case TokenType::PERCENT: return "PERCENT";
        case TokenType::PLUS_PLUS: return "PLUS_PLUS";
        case TokenType::MINUS_MINUS: return "MINUS_MINUS";
        case TokenType::PLUS_EQ: return "PLUS_EQ";
        case TokenType::MINUS_EQ: return "MINUS_EQ";
        case TokenType::STAR_EQ: return "STAR_EQ";
        case TokenType::SLASH_EQ: return "SLASH_EQ";
        case TokenType::PERCENT_EQ: return "PERCENT_EQ";
        case TokenType::EQ: return "EQ";
        case TokenType::EQEQ: return "EQEQ";
        case TokenType::NE: return "NE";
        case TokenType::LT: return "LT";
        case TokenType::GT: return "GT";
        case TokenType::LE: return "LE";
        case TokenType::GE: return "GE";
        case TokenType::AND: return "AND";
        case TokenType::OR: return "OR";
        case TokenType::NOT: return "NOT";
        case TokenType::AMPERSAND: return "AMPERSAND";
        case TokenType::PIPE: return "PIPE";
        case TokenType::CARET: return "CARET";
        case TokenType::TILDE: return "TILDE";
        case TokenType::LSHIFT: return "LSHIFT";
        case TokenType::RSHIFT: return "RSHIFT";
        case TokenType::AMP_EQ: return "AMP_EQ";
        case TokenType::PIPE_EQ: return "PIPE_EQ";
        case TokenType::CARET_EQ: return "CARET_EQ";
        case TokenType::LSHIFT_EQ: return "LSHIFT_EQ";
        case TokenType::RSHIFT_EQ: return "RSHIFT_EQ";
        // Delimiters
        case TokenType::LBRACE: return "LBRACE";
        case TokenType::RBRACE: return "RBRACE";
        case TokenType::LPAREN: return "LPAREN";
        case TokenType::RPAREN: return "RPAREN";
        case TokenType::LBRACKET: return "LBRACKET";
        case TokenType::RBRACKET: return "RBRACKET";
        case TokenType::SEMICOLON: return "SEMICOLON";
        case TokenType::COMMA: return "COMMA";
        case TokenType::DOT: return "DOT";
        case TokenType::RANGE: return "RANGE";
        case TokenType::COLON: return "COLON";
        case TokenType::QUESTION: return "QUESTION";
        case TokenType::ARROW: return "ARROW";
        case TokenType::ELLIPSIS: return "ELLIPSIS";
        // Literals
        case TokenType::INT_LIT: return "INT_LIT";
        case TokenType::FLOAT_LIT: return "FLOAT_LIT";
        case TokenType::STRING_LIT: return "STRING_LIT";
        case TokenType::CHAR_LIT: return "CHAR_LIT";
        case TokenType::IDENT: return "IDENT";
        // Special
        case TokenType::EOF_TOKEN: return "EOF";
        case TokenType::PRAGMA: return "PRAGMA";
        case TokenType::UNKNOWN: return "UNKNOWN";
        // No default: -Wswitch flags any TokenType that is missing a case here.
    }
    return "???";
}
