#pragma once

#include "roxy/core/types.hpp"

#include <cstring>
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
    // One or two character tokens.
    Bang, BangEqual,
    Equal, EqualEqual,
    Greater, GreaterEqual,
    Less, LessEqual,
    // Literals.
    Identifier, String, Number,
    // Keywords.
    And, Class, Else, False,
    For, Fun, If, Nil, Or,
    Return, Super, This,
    True, Var, While,

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

class Scanner {
public:
    Scanner(const u8* source);

    Token scan_token();

    bool is_at_end() const {
        return *m_current == '\0';
    }

private:
    u8 advance() {
        m_current++;
        return m_current[-1];
    }

    u8 peek() const {
        return *m_current;
    }

    u8 peek_next() const {
        if (is_at_end()) return '\0';
        return m_current[1];
    }

    bool match(u8 expected) {
        if (is_at_end()) return false;
        if (*m_current != expected) return false;
        m_current++;
        return true;
    }

    Token make_token(TokenType type) const {
        return Token(m_start, m_current - m_start, type, m_line);
    }

    Token error_token(const char* message) const {
        return Token(message, TokenType::Error, m_line);
    }

    void skip_whitespace();

    TokenType check_keyword(int32_t start, int32_t length, const char* rest, TokenType type) {
        if (m_current - m_start == start + length &&
            memcmp(m_start + start, rest, length) == 0) {
            return type;
        }
        return TokenType::Identifier;
    }

    TokenType identifier_type();

    Token identifier();

    Token number();

    Token string();

private:
    const u8* m_source;
    const u8* m_start;
    const u8* m_current;
    u32 m_line;
};

}