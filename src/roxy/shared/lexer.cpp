#include "roxy/shared/lexer.hpp"

#include <cstring>

namespace rx {

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool is_binary_digit(char c) {
    return c == '0' || c == '1';
}

static bool is_octal_digit(char c) {
    return c >= '0' && c <= '7';
}

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_';
}

static bool is_alpha_numeric(char c) {
    return is_alpha(c) || is_digit(c);
}

static i64 hex_char_to_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

Lexer::Lexer(const char* source, u32 length)
    : m_source(source)
    , m_length(length)
    , m_start(0)
    , m_current(0)
    , m_line(1)
    , m_line_start(0)
{}

Lexer::SavedPosition Lexer::save_position() const {
    return {m_start, m_current, m_line, m_line_start, m_fstring_brace_depth};
}

void Lexer::restore_position(const SavedPosition& pos) {
    m_start = pos.start;
    m_current = pos.current;
    m_line = pos.line;
    m_line_start = pos.line_start;
    m_fstring_brace_depth = pos.fstring_brace_depth;
}

bool Lexer::is_at_end() const {
    return m_current >= m_length;
}

char Lexer::advance() {
    return m_source[m_current++];
}

char Lexer::peek() const {
    if (is_at_end()) return '\0';
    return m_source[m_current];
}

char Lexer::peek_next() const {
    if (m_current + 1 >= m_length) return '\0';
    return m_source[m_current + 1];
}

bool Lexer::match(char expected) {
    if (is_at_end()) return false;
    if (m_source[m_current] != expected) return false;
    m_current++;
    return true;
}

void Lexer::skip_line_comment() {
    while (peek() != '\n' && !is_at_end()) {
        advance();
    }
}

void Lexer::skip_block_comment() {
    int nesting = 1;
    while (nesting > 0 && !is_at_end()) {
        if (peek() == '/' && peek_next() == '*') {
            advance();
            advance();
            nesting++;
        } else if (peek() == '*' && peek_next() == '/') {
            advance();
            advance();
            nesting--;
        } else {
            if (peek() == '\n') {
                m_line++;
                m_line_start = m_current + 1;
            }
            advance();
        }
    }
}

void Lexer::skip_whitespace() {
    for (;;) {
        char c = peek();
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
                advance();
                break;
            case '\n':
                m_line++;
                advance();
                m_line_start = m_current;
                break;
            case '/':
                if (peek_next() == '/') {
                    skip_line_comment();
                } else if (peek_next() == '*') {
                    advance();
                    advance();
                    skip_block_comment();
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

Token Lexer::make_token(TokenKind kind) {
    Token token;
    token.kind = kind;
    token.loc.offset = m_start;
    token.loc.line = m_line;
    token.loc.column = m_start - m_line_start + 1;
    token.start = m_source + m_start;
    token.length = m_current - m_start;
    token.int_value = 0;
    return token;
}

Token Lexer::error_token(const char* message) {
    Token token;
    token.kind = TokenKind::Error;
    token.loc.offset = m_start;
    token.loc.line = m_line;
    token.loc.column = m_start - m_line_start + 1;
    token.start = message;
    token.length = (u32)strlen(message);
    token.int_value = 0;
    return token;
}

TokenKind Lexer::check_keyword(u32 start, u32 len, const char* rest, TokenKind kind) {
    if (m_current - m_start == start + len &&
        memcmp(m_source + m_start + start, rest, len) == 0) {
        return kind;
    }
    return TokenKind::Identifier;
}

TokenKind Lexer::identifier_type() {
    switch (m_source[m_start]) {
        case 'b': return check_keyword(1, 4, "reak", TokenKind::KwBreak);
        case 'c':
            if (m_current - m_start > 1) {
                switch (m_source[m_start + 1]) {
                    case 'a': return check_keyword(2, 2, "se", TokenKind::KwCase);
                    case 'o': return check_keyword(2, 6, "ntinue", TokenKind::KwContinue);
                }
            }
            break;
        case 'd': return check_keyword(1, 5, "elete", TokenKind::KwDelete);
        case 'e':
            if (m_current - m_start > 1) {
                switch (m_source[m_start + 1]) {
                    case 'l': return check_keyword(2, 2, "se", TokenKind::KwElse);
                    case 'n': return check_keyword(2, 2, "um", TokenKind::KwEnum);
                }
            }
            break;
        case 'f':
            if (m_current - m_start > 1) {
                switch (m_source[m_start + 1]) {
                    case 'a': return check_keyword(2, 3, "lse", TokenKind::KwFalse);
                    case 'o': return check_keyword(2, 1, "r", TokenKind::KwFor);
                    case 'r': return check_keyword(2, 2, "om", TokenKind::KwFrom);
                    case 'u': return check_keyword(2, 1, "n", TokenKind::KwFun);
                }
            }
            break;
        case 'i':
            if (m_current - m_start > 1) {
                switch (m_source[m_start + 1]) {
                    case 'f': return check_keyword(2, 0, "", TokenKind::KwIf);
                    case 'm': return check_keyword(2, 4, "port", TokenKind::KwImport);
                    case 'n': return check_keyword(2, 3, "out", TokenKind::KwInout);
                }
            }
            break;
        case 'n':
            if (m_current - m_start > 1) {
                switch (m_source[m_start + 1]) {
                    case 'a': return check_keyword(2, 4, "tive", TokenKind::KwNative);
                    case 'e': return check_keyword(2, 1, "w", TokenKind::KwNew);
                    case 'i': return check_keyword(2, 1, "l", TokenKind::KwNil);
                }
            }
            break;
        case 'o': return check_keyword(1, 2, "ut", TokenKind::KwOut);
        case 'p': return check_keyword(1, 2, "ub", TokenKind::KwPub);
        case 'r':
            if (m_current - m_start > 1) {
                switch (m_source[m_start + 1]) {
                    case 'e':
                        if (m_current - m_start > 2) {
                            switch (m_source[m_start + 2]) {
                                case 'f': return check_keyword(3, 0, "", TokenKind::KwRef);
                                case 't': return check_keyword(3, 3, "urn", TokenKind::KwReturn);
                            }
                        }
                        break;
                }
            }
            break;
        case 's':
            if (m_current - m_start > 1) {
                switch (m_source[m_start + 1]) {
                    case 'e': return check_keyword(2, 2, "lf", TokenKind::KwSelf);
                    case 't': return check_keyword(2, 4, "ruct", TokenKind::KwStruct);
                    case 'u': return check_keyword(2, 3, "per", TokenKind::KwSuper);
                }
            }
            break;
        case 't':
            if (m_current - m_start > 1) {
                switch (m_source[m_start + 1]) {
                    case 'r':
                        if (m_current - m_start > 2) {
                            switch (m_source[m_start + 2]) {
                                case 'u': return check_keyword(3, 1, "e", TokenKind::KwTrue);
                                case 'a': return check_keyword(3, 2, "it", TokenKind::KwTrait);
                            }
                        }
                        break;
                }
            }
            break;
        case 'u': return check_keyword(1, 3, "niq", TokenKind::KwUniq);
        case 'v': return check_keyword(1, 2, "ar", TokenKind::KwVar);
        case 'w':
            if (m_current - m_start > 1) {
                switch (m_source[m_start + 1]) {
                    case 'e': return check_keyword(2, 2, "ak", TokenKind::KwWeak);
                    case 'h':
                        if (m_current - m_start > 2) {
                            switch (m_source[m_start + 2]) {
                                case 'e': return check_keyword(3, 1, "n", TokenKind::KwWhen);
                                case 'i': return check_keyword(3, 2, "le", TokenKind::KwWhile);
                            }
                        }
                        break;
                }
            }
            break;
    }
    return TokenKind::Identifier;
}

Token Lexer::scan_identifier() {
    while (is_alpha_numeric(peek())) {
        advance();
    }
    return make_token(identifier_type());
}

Token Lexer::scan_number() {
    Token token;
    token.loc.offset = m_start;
    token.loc.line = m_line;
    token.loc.column = m_start - m_line_start + 1;
    token.start = m_source + m_start;

    bool is_float = false;
    i64 int_value = 0;
    f64 float_value = 0.0;

    // Check for hex, binary, or octal prefix
    if (m_source[m_start] == '0' && m_current - m_start == 1) {
        char prefix = peek();
        if (prefix == 'x' || prefix == 'X') {
            // Hexadecimal
            advance();
            if (!is_hex_digit(peek())) {
                return error_token("Invalid hexadecimal literal.");
            }
            while (is_hex_digit(peek())) {
                int_value = int_value * 16 + hex_char_to_value(advance());
            }
        } else if (prefix == 'b' || prefix == 'B') {
            // Binary
            advance();
            if (!is_binary_digit(peek())) {
                return error_token("Invalid binary literal.");
            }
            while (is_binary_digit(peek())) {
                int_value = int_value * 2 + (advance() - '0');
            }
        } else if (prefix == 'o' || prefix == 'O') {
            // Octal
            advance();
            if (!is_octal_digit(peek())) {
                return error_token("Invalid octal literal.");
            }
            while (is_octal_digit(peek())) {
                int_value = int_value * 8 + (advance() - '0');
            }
        } else {
            // Just a zero or decimal starting with 0
            goto decimal;
        }
        goto suffixes;
    }

decimal:
    // Decimal integer part
    while (is_digit(peek())) {
        advance();
    }

    // Parse integer value so far
    for (u32 i = m_start; i < m_current; i++) {
        int_value = int_value * 10 + (m_source[i] - '0');
    }

    // Look for a fractional part
    if (peek() == '.' && is_digit(peek_next())) {
        is_float = true;
        advance(); // consume '.'

        float_value = (f64)int_value;
        f64 fraction = 0.1;
        while (is_digit(peek())) {
            float_value += (advance() - '0') * fraction;
            fraction *= 0.1;
        }
    }

suffixes:
    // Check for suffixes: u, l, ul, f
    if (peek() == 'f' || peek() == 'F') {
        is_float = true;
        advance();
        if (!is_float) {
            float_value = (f64)int_value;
        }
    } else if (peek() == 'u' || peek() == 'U') {
        advance();
        if (peek() == 'l' || peek() == 'L') {
            advance();
        }
    } else if (peek() == 'l' || peek() == 'L') {
        advance();
    }

    token.length = m_current - m_start;

    if (is_float) {
        token.kind = TokenKind::FloatLiteral;
        token.float_value = float_value;
    } else {
        token.kind = TokenKind::IntLiteral;
        token.int_value = int_value;
    }

    return token;
}

Token Lexer::scan_string() {
    while (peek() != '"' && !is_at_end()) {
        if (peek() == '\n') {
            m_line++;
            m_line_start = m_current + 1;
        }
        if (peek() == '\\' && peek_next() != '\0') {
            // Skip escape sequence
            advance();
        }
        advance();
    }

    if (is_at_end()) {
        return error_token("Unterminated string.");
    }

    // Closing quote
    advance();
    return make_token(TokenKind::StringLiteral);
}

Token Lexer::scan_fstring_text(bool is_begin) {
    // m_start was set before advance() was called on the 'f' or '}'
    // For begin: we consumed 'f' and '"', now scan text until '{' or '"'
    // For mid/end: we consumed '}', now scan text until '{' or '"'

    // Record start of the text content for the token
    m_start = is_begin ? m_current - 2 : m_current - 1;  // include f" or }

    while (!is_at_end()) {
        char c = peek();

        if (c == '\\' && !is_at_end()) {
            // Skip escape sequence (including \{ and \})
            advance();
            if (!is_at_end()) advance();
            continue;
        }

        if (c == '{') {
            // Start of interpolation
            m_fstring_brace_depth = 1;
            advance();  // consume '{'
            TokenKind kind = is_begin ? TokenKind::FStringBegin : TokenKind::FStringMid;
            return make_token(kind);
        }

        if (c == '"') {
            advance();  // consume closing quote
            if (is_begin) {
                // f"plain text" with no interpolation — degenerate to StringLiteral
                // Token text is f"...text..." including the f prefix
                // We need to return this as a StringLiteral with just the "...text..." part
                // Adjust m_start to skip the 'f' prefix
                Token token;
                token.kind = TokenKind::StringLiteral;
                token.loc.offset = m_start + 1;  // skip 'f'
                token.loc.line = m_line;
                token.loc.column = m_start + 1 - m_line_start + 1;
                token.start = m_source + m_start + 1;  // skip 'f'
                token.length = m_current - m_start - 1;  // exclude 'f'
                token.int_value = 0;
                return token;
            }
            return make_token(TokenKind::FStringEnd);
        }

        if (c == '\n') {
            m_line++;
            m_line_start = m_current + 1;
        }

        advance();
    }

    return error_token("Unterminated f-string.");
}

Token Lexer::next_token() {
    skip_whitespace();

    m_start = m_current;

    if (is_at_end()) {
        return make_token(TokenKind::Eof);
    }

    char c = advance();

    // F-string: f"..."
    if (c == 'f' && peek() == '"') {
        advance();  // consume opening quote
        return scan_fstring_text(true);
    }

    // Track braces inside f-string expressions
    if (m_fstring_brace_depth > 0) {
        if (c == '{') {
            m_fstring_brace_depth++;
            return make_token(TokenKind::LeftBrace);
        }
        if (c == '}') {
            m_fstring_brace_depth--;
            if (m_fstring_brace_depth == 0) {
                return scan_fstring_text(false);  // scan next text segment
            }
            return make_token(TokenKind::RightBrace);
        }
    }

    if (is_alpha(c)) return scan_identifier();
    if (is_digit(c)) return scan_number();

    switch (c) {
        case '(': return make_token(TokenKind::LeftParen);
        case ')': return make_token(TokenKind::RightParen);
        case '{': return make_token(TokenKind::LeftBrace);
        case '}': return make_token(TokenKind::RightBrace);
        case '[': return make_token(TokenKind::LeftBracket);
        case ']': return make_token(TokenKind::RightBracket);
        case ',': return make_token(TokenKind::Comma);
        case '.': return make_token(TokenKind::Dot);
        case ';': return make_token(TokenKind::Semicolon);
        case '?': return make_token(TokenKind::Question);
        case '~': return make_token(TokenKind::Tilde);

        case ':':
            return make_token(match(':') ? TokenKind::ColonColon : TokenKind::Colon);
        case '+':
            return make_token(match('=') ? TokenKind::PlusEqual : TokenKind::Plus);
        case '-':
            return make_token(match('=') ? TokenKind::MinusEqual : TokenKind::Minus);
        case '*':
            return make_token(match('=') ? TokenKind::StarEqual : TokenKind::Star);
        case '/':
            return make_token(match('=') ? TokenKind::SlashEqual : TokenKind::Slash);
        case '%':
            return make_token(match('=') ? TokenKind::PercentEqual : TokenKind::Percent);
        case '!':
            return make_token(match('=') ? TokenKind::BangEqual : TokenKind::Bang);
        case '=':
            return make_token(match('=') ? TokenKind::EqualEqual : TokenKind::Equal);
        case '<':
            if (match('<')) {
                return make_token(match('=') ? TokenKind::LessLessEqual : TokenKind::LessLess);
            }
            return make_token(match('=') ? TokenKind::LessEqual : TokenKind::Less);
        case '>':
            if (match('>')) {
                return make_token(match('=') ? TokenKind::GreaterGreaterEqual : TokenKind::GreaterGreater);
            }
            return make_token(match('=') ? TokenKind::GreaterEqual : TokenKind::Greater);
        case '&':
            if (match('&')) return make_token(TokenKind::AmpAmp);
            if (match('=')) return make_token(TokenKind::AmpEqual);
            return make_token(TokenKind::Amp);
        case '|':
            if (match('|')) return make_token(TokenKind::PipePipe);
            if (match('=')) return make_token(TokenKind::PipeEqual);
            return make_token(TokenKind::Pipe);
        case '^':
            return make_token(match('=') ? TokenKind::CaretEqual : TokenKind::Caret);

        case '"': return scan_string();
    }

    return error_token("Unexpected character.");
}

}
