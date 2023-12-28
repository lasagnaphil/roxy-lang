#include "roxy/scanner.hpp"
#include "roxy/core/binary_search.hpp"

#include <cstring>

namespace rx {
bool Scanner::identifiers_equal(const Token& a, const Token& b) {
    if (a.length != b.length) return false;
    return memcmp(m_source + a.source_loc, m_source + b.source_loc, a.length) == 0;
}

inline bool is_alpha(u8 c) {
    return (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        c == '_';
}

inline bool is_digit(u8 c) {
    return c >= '0' && c <= '9';
}

Scanner::Scanner(const u8* source) {
    m_source = source;
    m_start = source;
    m_current = source;
    m_line_start.push_back(0);
}

u32 Scanner::get_line(SourceLocation loc) const {
    if (m_line_start.size() == 1) return 1;
    return binary_search(m_line_start.data(), m_line_start.size(), loc.source_loc) + 1;
}

Token Scanner::scan_token() {
    skip_whitespace();

    m_start = m_current;
    if (is_at_end()) return make_token(TokenType::Eof);

    char c = advance();
    if (is_alpha(c)) return identifier();
    if (is_digit(c)) return number();

    switch (c) {
    case '(': return make_token(TokenType::LeftParen);
    case ')': return make_token(TokenType::RightParen);
    case '{': return make_token(TokenType::LeftBrace);
    case '}': return make_token(TokenType::RightBrace);
    case '[': return make_token(TokenType::LeftBracket);
    case ']': return make_token(TokenType::RightBracket);
    case ';': return make_token(TokenType::Semicolon);
    case ',': return make_token(TokenType::Comma);
    case '.': return make_token(TokenType::Dot);
    case '-': return make_token(TokenType::Minus);
    case '+': return make_token(TokenType::Plus);
    case '/': return make_token(TokenType::Slash);
    case '*': return make_token(TokenType::Star);
    case '%': return make_token(TokenType::Percent);
    case '?': return make_token(TokenType::QuestionMark);
    case ':': return make_token(TokenType::Colon);
    case '~': return make_token(TokenType::Tilde);
    case '^': return make_token(TokenType::Caret);
    case '!': return make_token(match('=') ? TokenType::BangEqual : TokenType::Bang);
    case '=': return make_token(match('=') ? TokenType::EqualEqual : TokenType::Equal);
    case '<': return make_token(match('=') ? TokenType::LessEqual : TokenType::Less);
    case '>': return make_token(match('=') ? TokenType::GreaterEqual : TokenType::Greater);
    case '&': return make_token(match('&') ? TokenType::AmpAmp : TokenType::Ampersand);
    case '|': return make_token(match('|') ? TokenType::BarBar : TokenType::Bar);
    case '"': return string();
    }
    return make_error_token(TokenType::ErrorUnexpectedCharacter);
}

void Scanner::skip_whitespace() {
    for (;;) {
        char c = peek();
        switch (c) {
        case ' ':
        case '\r':
        case '\t':
            advance();
            break;
        case '\n':
            new_line();
            advance();
            break;
        case '/':
            if (peek_next() == '/') {
                while (peek() != '\n' && !is_at_end()) advance();
            }
            else {
                return;
            }
            break;
        default:
            return;
        }
    }
}

TokenType Scanner::identifier_type() {
    switch (m_start[0]) {
    case 'e': return check_keyword(1, 3, "lse", TokenType::Else);
    case 'f':
        if (m_current - m_start > 1) {
            switch (m_start[1]) {
            case 'a': return check_keyword(2, 3, "lse", TokenType::False);
            case 'o': return check_keyword(2, 1, "r", TokenType::For);
            case 'u': return check_keyword(2, 1, "n", TokenType::Fun);
            }
        }
        break;
    case 'i': return check_keyword(1, 1, "f", TokenType::If);
    case 'n':
        if (m_current - m_start > 1) {
            switch (m_start[1]) {
                case 'a': return check_keyword(2, 4, "tive", TokenType::Native);
                case 'i': return check_keyword(2, 1, "l", TokenType::Nil);
            }
        }
    case 'r': return check_keyword(1, 5, "eturn", TokenType::Return);
    case 's':
        if (m_current - m_start > 1) {
            switch (m_start[1]) {
            case 't': return check_keyword(2, 4, "ruct", TokenType::Struct);
            case 'u': return check_keyword(2, 3, "per", TokenType::Super);
            }
        }
        break;
    case 't':
        if (m_current - m_start > 1) {
            switch (m_start[1]) {
            case 'h': return check_keyword(2, 2, "is", TokenType::This);
            case 'r': return check_keyword(2, 2, "ue", TokenType::True);
            }
        }
    case 'v': return check_keyword(1, 2, "ar", TokenType::Var);
    case 'w': return check_keyword(1, 4, "hile", TokenType::While);
    }

    return TokenType::Identifier;
}

Token Scanner::identifier() {
    while (is_alpha(peek()) || is_digit(peek())) advance();
    return make_token(identifier_type());
}

Token Scanner::number() {
    while (is_digit(peek())) advance();

    if (peek() == '.' && is_digit(peek_next())) {
        advance();
        while (is_digit(peek())) advance();
        if (peek() == 'f' || peek() == 'F' || peek() == 'd' || peek() == 'D') {
            advance();
        }
        return make_token(TokenType::NumberFloat);
    }
    else {
        if (peek() == 'u' || peek() == 'U') {
            advance();
            if (peek() == 'l' || peek() == 'L') advance();
        }
        else if (peek() == 'i' || peek() == 'I') {
            advance();
            if (peek() == 'l' || peek() == 'L') advance();
        }
        else if (peek() == 'l' || peek() == 'L') advance();
        return make_token(TokenType::NumberInt);
    }
}

Token Scanner::string() {
    while (peek() != '"' && !is_at_end()) {
        if (peek() == '\n') new_line();
        advance();
    }
    if (is_at_end()) return make_error_token(TokenType::ErrorUnterminatedString);
    advance();
    return make_token(TokenType::String);
}
}
