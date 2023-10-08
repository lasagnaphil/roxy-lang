#pragma once

#include "roxy/core/vector.hpp"
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

    const u8* source() { return m_source; }

    u32 get_line(Token token) const {
        return get_line(SourceLocation{token.source_loc, token.length});
    }
    u32 get_line(SourceLocation loc) const;

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

    void new_line() {
        m_line_start.push_back(m_start - m_source);
    }

    Token make_token(TokenType type) const {
        return Token(m_start - m_source, m_current - m_start, type);
    }

    Token make_error_token(TokenType type) const {
        return Token(m_start - m_source, type);
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
    bool identifiers_equal(const Token& a, const Token& b);

    const u8* m_source;
    const u8* m_start;
    const u8* m_current;
    Vector<u32> m_line_start;
};

}