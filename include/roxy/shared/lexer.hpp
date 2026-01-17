#pragma once

#include "roxy/core/types.hpp"
#include "roxy/shared/token.hpp"

namespace rx {

class Lexer {
public:
    Lexer(const char* source, u32 length);

    Token next_token();
    bool is_at_end() const;

    const char* source() const { return m_source; }
    u32 length() const { return m_length; }

private:
    const char* m_source;
    u32 m_length;
    u32 m_start;        // Start of current token
    u32 m_current;      // Current position
    u32 m_line;
    u32 m_line_start;   // Offset where current line started

    char advance();
    char peek() const;
    char peek_next() const;
    bool match(char expected);
    void skip_whitespace();
    void skip_line_comment();
    void skip_block_comment();

    Token make_token(TokenKind kind);
    Token error_token(const char* message);

    Token scan_identifier();
    Token scan_number();
    Token scan_string();

    TokenKind check_keyword(u32 start, u32 len, const char* rest, TokenKind kind);
    TokenKind identifier_type();
};

}
