#pragma once

#include "roxy/core/types.hpp"

#include <string_view>

namespace rx {

enum class TokenType : u8 {
    // Single-character tokens.
    LeftParen, RightParen,
    LeftBrace, RightBrace,
    LeftBracket, RightBracket,
    Comma, Dot, Minus, Plus,
    Semicolon, Slash, Star,
    QuestionMark, Colon,
    Ampersand, Bar, Tilde, Caret,
    // One or two character tokens.
    Bang, BangEqual,
    Equal, EqualEqual,
    Greater, GreaterEqual,
    Less, LessEqual,
    // Two character tokens.
    AmpAmp, BarBar,
    // Literals.
    Identifier, String, Number,
    // Keywords.
    Struct, Else, False,
    For, Fun, If, Nil,
    Return, Super, This, Print,
    True, Var, While,
    Break, Continue,

    Error, Eof
};

struct Token {
    const u8* start;
    u32 line;
    u16 length;
    TokenType type;

    Token() = default;

    Token(const u8* start, u16 length, TokenType type, u32 line) :
            start(start), length(length), type(type), line(line) {}

    Token(std::string_view name, TokenType type, u32 line) :
            start(reinterpret_cast<const u8*>(name.data())),
            length(name.length()),
            type(type),
            line(line) {}

    std::string_view str() const { return {(const char*) start, length}; }
};

}