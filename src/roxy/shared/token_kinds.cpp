#include "roxy/shared/token_kinds.hpp"

namespace rx {

const char* token_kind_to_string(TokenKind kind) {
    switch (kind) {
        // Literals
        case TokenKind::IntLiteral:     return "IntLiteral";
        case TokenKind::FloatLiteral:   return "FloatLiteral";
        case TokenKind::StringLiteral:  return "StringLiteral";
        case TokenKind::Identifier:     return "Identifier";

        // Single-character tokens
        case TokenKind::LeftParen:      return "(";
        case TokenKind::RightParen:     return ")";
        case TokenKind::LeftBrace:      return "{";
        case TokenKind::RightBrace:     return "}";
        case TokenKind::LeftBracket:    return "[";
        case TokenKind::RightBracket:   return "]";
        case TokenKind::Comma:          return ",";
        case TokenKind::Dot:            return ".";
        case TokenKind::Semicolon:      return ";";
        case TokenKind::Colon:          return ":";
        case TokenKind::Question:       return "?";
        case TokenKind::Tilde:          return "~";

        // One or two character tokens
        case TokenKind::Plus:           return "+";
        case TokenKind::PlusEqual:      return "+=";
        case TokenKind::Minus:          return "-";
        case TokenKind::MinusEqual:     return "-=";
        case TokenKind::Star:           return "*";
        case TokenKind::StarEqual:      return "*=";
        case TokenKind::Slash:          return "/";
        case TokenKind::SlashEqual:     return "/=";
        case TokenKind::Percent:        return "%";
        case TokenKind::PercentEqual:   return "%=";
        case TokenKind::Bang:           return "!";
        case TokenKind::BangEqual:      return "!=";
        case TokenKind::Equal:          return "=";
        case TokenKind::EqualEqual:     return "==";
        case TokenKind::Less:           return "<";
        case TokenKind::LessEqual:      return "<=";
        case TokenKind::Greater:        return ">";
        case TokenKind::GreaterEqual:   return ">=";
        case TokenKind::Amp:            return "&";
        case TokenKind::AmpAmp:         return "&&";
        case TokenKind::Pipe:           return "|";
        case TokenKind::PipePipe:       return "||";
        case TokenKind::ColonColon:     return "::";

        // Keywords - Types/modifiers
        case TokenKind::KwTrue:         return "true";
        case TokenKind::KwFalse:        return "false";
        case TokenKind::KwNil:          return "nil";
        case TokenKind::KwVar:          return "var";
        case TokenKind::KwFun:          return "fun";
        case TokenKind::KwStruct:       return "struct";
        case TokenKind::KwEnum:         return "enum";
        case TokenKind::KwPub:          return "pub";
        case TokenKind::KwNative:       return "native";

        // Keywords - Control flow
        case TokenKind::KwIf:           return "if";
        case TokenKind::KwElse:         return "else";
        case TokenKind::KwFor:          return "for";
        case TokenKind::KwWhile:        return "while";
        case TokenKind::KwBreak:        return "break";
        case TokenKind::KwContinue:     return "continue";
        case TokenKind::KwReturn:       return "return";
        case TokenKind::KwWhen:         return "when";
        case TokenKind::KwCase:         return "case";

        // Keywords - OOP
        case TokenKind::KwThis:         return "this";
        case TokenKind::KwSuper:        return "super";
        case TokenKind::KwNew:          return "new";
        case TokenKind::KwDelete:       return "delete";

        // Keywords - References
        case TokenKind::KwUniq:         return "uniq";
        case TokenKind::KwRef:          return "ref";
        case TokenKind::KwWeak:         return "weak";
        case TokenKind::KwOut:          return "out";
        case TokenKind::KwInout:        return "inout";

        // Keywords - Imports
        case TokenKind::KwImport:       return "import";
        case TokenKind::KwFrom:         return "from";

        // Special tokens
        case TokenKind::Error:          return "Error";
        case TokenKind::Eof:            return "Eof";
    }
    return "Unknown";
}

}
