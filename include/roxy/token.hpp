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
    Semicolon, Slash, Star, Percent,
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
    Identifier, String, NumberInt, NumberFloat,
    // Keywords.
    Struct, Else, False,
    For, Fun, If, Nil, Native,
    Return, Super, This,
    True, Var, While,
    Break, Continue,

    Eof,

    ErrorUnexpectedCharacter = 0b10000000,
    ErrorUnterminatedString,
};


struct SourceLocation {
    u32 source_loc;
    u16 length;

    static SourceLocation from_start_end(u32 start, u32 end) {
        u32 length = end - start;
        assert(length <= UINT16_MAX);
        return {start, (u16)length};
    }
};

struct Token {
    u32 source_loc;
    u16 length;
    TokenType type;

    Token() = default;

    Token(u32 source_loc, u16 length, TokenType type) :
            source_loc(source_loc), length(length), type(type) {}

    Token(SourceLocation loc, TokenType type) :
            source_loc(loc.source_loc), length(loc.length), type(type) {}

    Token(u32 source_loc, TokenType type) :
            source_loc(source_loc), length(0), type(type) {
        assert(is_error());
    }

    bool is_error() const {
        return ((u8)type & 0b10000000) != 0;
    }

    bool is_arithmetic() const {
        return type == TokenType::Plus || type == TokenType::Minus || type == TokenType::Star ||
            type == TokenType::Slash || type == TokenType::Percent;
    }

    std::string_view str(const u8* source) const {
        return {reinterpret_cast<const char*>(source + source_loc), length};
    }

    SourceLocation get_source_loc() const {
        return {source_loc, length};
    }
};

}