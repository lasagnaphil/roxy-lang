#pragma once

#include "roxy/core/types.hpp"

#include <string_view>
#include <cassert>

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

    Eof,

    ErrorUnexpectedCharacter = 0b10000000,
    ErrorUnterminatedString,
};

struct Token {
    u32 source_loc;
    u16 length;
    TokenType type;

    Token() = default;

    Token(u32 source_loc, u16 length, TokenType type) :
            source_loc(source_loc), length(length), type(type) {}

    Token(u32 source_loc, TokenType type) :
            source_loc(source_loc), length(0), type(type) {
        assert(is_error());
    }

    bool is_error() const {
        return ((u8)type & 0b10000000) != 0;
    }

    std::string_view str(const u8* source) const {
        return {reinterpret_cast<const char*>(source + source_loc), length};
    }
};

}