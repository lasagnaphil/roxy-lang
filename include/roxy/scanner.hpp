#pragma once

#include "roxy/token.hpp"

#include <cstring>

namespace rx {

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