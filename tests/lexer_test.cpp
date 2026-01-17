#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "roxy/core/doctest/doctest.h"

#include "roxy/shared/lexer.hpp"
#include "roxy/core/vector.hpp"

#include <cstring>

using namespace rx;

// Helper to collect all tokens from source
Vector<Token> lex_all(const char* source) {
    Vector<Token> tokens;
    Lexer lexer(source, (u32)strlen(source));
    while (true) {
        Token token = lexer.next_token();
        tokens.push_back(token);
        if (token.kind == TokenKind::Eof || token.kind == TokenKind::Error) {
            break;
        }
    }
    return tokens;
}

// Helper to check token kind at index
void check_token(const Vector<Token>& tokens, u32 index, TokenKind expected_kind) {
    REQUIRE(index < tokens.size());
    CHECK(tokens[index].kind == expected_kind);
}

// Helper to check int literal
void check_int_literal(const Vector<Token>& tokens, u32 index, i64 expected_value) {
    REQUIRE(index < tokens.size());
    CHECK(tokens[index].kind == TokenKind::IntLiteral);
    CHECK(tokens[index].int_value == expected_value);
}

// Helper to check float literal
void check_float_literal(const Vector<Token>& tokens, u32 index, f64 expected_value, f64 epsilon = 0.0001) {
    REQUIRE(index < tokens.size());
    CHECK(tokens[index].kind == TokenKind::FloatLiteral);
    CHECK(tokens[index].float_value == doctest::Approx(expected_value).epsilon(epsilon));
}

// Helper to check identifier
void check_identifier(const Vector<Token>& tokens, u32 index, const char* expected_name) {
    REQUIRE(index < tokens.size());
    CHECK(tokens[index].kind == TokenKind::Identifier);
    CHECK(tokens[index].length == strlen(expected_name));
    CHECK(memcmp(tokens[index].start, expected_name, tokens[index].length) == 0);
}

// Helper to check token location
void check_location(const Vector<Token>& tokens, u32 index, u32 line, u32 column) {
    REQUIRE(index < tokens.size());
    CHECK(tokens[index].loc.line == line);
    CHECK(tokens[index].loc.column == column);
}

TEST_CASE("Lexer: Keywords") {
    auto tokens = lex_all(
        "true false nil var fun struct enum pub native "
        "if else for while break continue return when case "
        "this super new delete "
        "uniq ref weak out inout "
        "import from"
    );

    u32 i = 0;
    // Types/modifiers
    check_token(tokens, i++, TokenKind::KwTrue);
    check_token(tokens, i++, TokenKind::KwFalse);
    check_token(tokens, i++, TokenKind::KwNil);
    check_token(tokens, i++, TokenKind::KwVar);
    check_token(tokens, i++, TokenKind::KwFun);
    check_token(tokens, i++, TokenKind::KwStruct);
    check_token(tokens, i++, TokenKind::KwEnum);
    check_token(tokens, i++, TokenKind::KwPub);
    check_token(tokens, i++, TokenKind::KwNative);

    // Control flow
    check_token(tokens, i++, TokenKind::KwIf);
    check_token(tokens, i++, TokenKind::KwElse);
    check_token(tokens, i++, TokenKind::KwFor);
    check_token(tokens, i++, TokenKind::KwWhile);
    check_token(tokens, i++, TokenKind::KwBreak);
    check_token(tokens, i++, TokenKind::KwContinue);
    check_token(tokens, i++, TokenKind::KwReturn);
    check_token(tokens, i++, TokenKind::KwWhen);
    check_token(tokens, i++, TokenKind::KwCase);

    // OOP
    check_token(tokens, i++, TokenKind::KwThis);
    check_token(tokens, i++, TokenKind::KwSuper);
    check_token(tokens, i++, TokenKind::KwNew);
    check_token(tokens, i++, TokenKind::KwDelete);

    // References
    check_token(tokens, i++, TokenKind::KwUniq);
    check_token(tokens, i++, TokenKind::KwRef);
    check_token(tokens, i++, TokenKind::KwWeak);
    check_token(tokens, i++, TokenKind::KwOut);
    check_token(tokens, i++, TokenKind::KwInout);

    // Imports
    check_token(tokens, i++, TokenKind::KwImport);
    check_token(tokens, i++, TokenKind::KwFrom);

    check_token(tokens, i++, TokenKind::Eof);
}

TEST_CASE("Lexer: Identifiers") {
    auto tokens = lex_all("foo _bar baz123 _123 camelCase snake_case PascalCase");

    check_identifier(tokens, 0, "foo");
    check_identifier(tokens, 1, "_bar");
    check_identifier(tokens, 2, "baz123");
    check_identifier(tokens, 3, "_123");
    check_identifier(tokens, 4, "camelCase");
    check_identifier(tokens, 5, "snake_case");
    check_identifier(tokens, 6, "PascalCase");
    check_token(tokens, 7, TokenKind::Eof);
}

TEST_CASE("Lexer: Integer Literals") {
    SUBCASE("Decimal integers") {
        auto tokens = lex_all("0 123 456789");
        check_int_literal(tokens, 0, 0);
        check_int_literal(tokens, 1, 123);
        check_int_literal(tokens, 2, 456789);
    }

    SUBCASE("Integer suffixes") {
        auto tokens = lex_all("456u 789l 1000ul 1000UL");
        check_int_literal(tokens, 0, 456);
        check_int_literal(tokens, 1, 789);
        check_int_literal(tokens, 2, 1000);
        check_int_literal(tokens, 3, 1000);
    }

    SUBCASE("Hexadecimal") {
        auto tokens = lex_all("0xFF 0xff 0x10 0xABCDEF");
        check_int_literal(tokens, 0, 255);
        check_int_literal(tokens, 1, 255);
        check_int_literal(tokens, 2, 16);
        check_int_literal(tokens, 3, 0xABCDEF);
    }

    SUBCASE("Binary") {
        auto tokens = lex_all("0b1010 0B1111 0b0 0b1");
        check_int_literal(tokens, 0, 10);
        check_int_literal(tokens, 1, 15);
        check_int_literal(tokens, 2, 0);
        check_int_literal(tokens, 3, 1);
    }

    SUBCASE("Octal") {
        auto tokens = lex_all("0o77 0O755 0o0 0o10");
        check_int_literal(tokens, 0, 63);
        check_int_literal(tokens, 1, 493);
        check_int_literal(tokens, 2, 0);
        check_int_literal(tokens, 3, 8);
    }
}

TEST_CASE("Lexer: Float Literals") {
    auto tokens = lex_all("1.5 3.14159 0.5 1.0f 2.5F");

    check_float_literal(tokens, 0, 1.5);
    check_float_literal(tokens, 1, 3.14159);
    check_float_literal(tokens, 2, 0.5);
    check_float_literal(tokens, 3, 1.0);
    check_float_literal(tokens, 4, 2.5);
}

TEST_CASE("Lexer: String Literals") {
    auto tokens = lex_all("\"hello\" \"world\" \"with\\nescapes\"");

    check_token(tokens, 0, TokenKind::StringLiteral);
    CHECK(tokens[0].length == 7);  // "hello" including quotes

    check_token(tokens, 1, TokenKind::StringLiteral);
    CHECK(tokens[1].length == 7);  // "world"

    check_token(tokens, 2, TokenKind::StringLiteral);
}

TEST_CASE("Lexer: Single Character Operators") {
    auto tokens = lex_all("( ) { } [ ] , . ; : ? ~");

    u32 i = 0;
    check_token(tokens, i++, TokenKind::LeftParen);
    check_token(tokens, i++, TokenKind::RightParen);
    check_token(tokens, i++, TokenKind::LeftBrace);
    check_token(tokens, i++, TokenKind::RightBrace);
    check_token(tokens, i++, TokenKind::LeftBracket);
    check_token(tokens, i++, TokenKind::RightBracket);
    check_token(tokens, i++, TokenKind::Comma);
    check_token(tokens, i++, TokenKind::Dot);
    check_token(tokens, i++, TokenKind::Semicolon);
    check_token(tokens, i++, TokenKind::Colon);
    check_token(tokens, i++, TokenKind::Question);
    check_token(tokens, i++, TokenKind::Tilde);
    check_token(tokens, i++, TokenKind::Eof);
}

TEST_CASE("Lexer: Multi Character Operators") {
    SUBCASE("Arithmetic operators") {
        auto tokens = lex_all("+ += - -= * *= / /= % %=");
        u32 i = 0;
        check_token(tokens, i++, TokenKind::Plus);
        check_token(tokens, i++, TokenKind::PlusEqual);
        check_token(tokens, i++, TokenKind::Minus);
        check_token(tokens, i++, TokenKind::MinusEqual);
        check_token(tokens, i++, TokenKind::Star);
        check_token(tokens, i++, TokenKind::StarEqual);
        check_token(tokens, i++, TokenKind::Slash);
        check_token(tokens, i++, TokenKind::SlashEqual);
        check_token(tokens, i++, TokenKind::Percent);
        check_token(tokens, i++, TokenKind::PercentEqual);
    }

    SUBCASE("Comparison operators") {
        auto tokens = lex_all("! != = == < <= > >=");
        u32 i = 0;
        check_token(tokens, i++, TokenKind::Bang);
        check_token(tokens, i++, TokenKind::BangEqual);
        check_token(tokens, i++, TokenKind::Equal);
        check_token(tokens, i++, TokenKind::EqualEqual);
        check_token(tokens, i++, TokenKind::Less);
        check_token(tokens, i++, TokenKind::LessEqual);
        check_token(tokens, i++, TokenKind::Greater);
        check_token(tokens, i++, TokenKind::GreaterEqual);
    }

    SUBCASE("Logical and scope operators") {
        auto tokens = lex_all("& && | || ::");
        u32 i = 0;
        check_token(tokens, i++, TokenKind::Amp);
        check_token(tokens, i++, TokenKind::AmpAmp);
        check_token(tokens, i++, TokenKind::Pipe);
        check_token(tokens, i++, TokenKind::PipePipe);
        check_token(tokens, i++, TokenKind::ColonColon);
    }
}

TEST_CASE("Lexer: Comments") {
    SUBCASE("Line comments") {
        auto tokens = lex_all("foo // this is a comment\nbar");
        check_identifier(tokens, 0, "foo");
        check_identifier(tokens, 1, "bar");
        check_token(tokens, 2, TokenKind::Eof);
    }

    SUBCASE("Block comments") {
        auto tokens = lex_all("foo /* block comment */ bar");
        check_identifier(tokens, 0, "foo");
        check_identifier(tokens, 1, "bar");
        check_token(tokens, 2, TokenKind::Eof);
    }

    SUBCASE("Nested block comments") {
        auto tokens = lex_all("foo /* outer /* inner */ outer */ bar");
        check_identifier(tokens, 0, "foo");
        check_identifier(tokens, 1, "bar");
        check_token(tokens, 2, TokenKind::Eof);
    }
}

TEST_CASE("Lexer: Line and Column Tracking") {
    auto tokens = lex_all("foo\nbar\n  baz");

    check_location(tokens, 0, 1, 1);  // foo at line 1, col 1
    check_location(tokens, 1, 2, 1);  // bar at line 2, col 1
    check_location(tokens, 2, 3, 3);  // baz at line 3, col 3
}

TEST_CASE("Lexer: Code Snippet - Function") {
    auto tokens = lex_all(
        "fun add(a: i32, b: i32): i32 {\n"
        "    return a + b;\n"
        "}"
    );

    u32 i = 0;
    check_token(tokens, i++, TokenKind::KwFun);
    check_identifier(tokens, i++, "add");
    check_token(tokens, i++, TokenKind::LeftParen);
    check_identifier(tokens, i++, "a");
    check_token(tokens, i++, TokenKind::Colon);
    check_identifier(tokens, i++, "i32");
    check_token(tokens, i++, TokenKind::Comma);
    check_identifier(tokens, i++, "b");
    check_token(tokens, i++, TokenKind::Colon);
    check_identifier(tokens, i++, "i32");
    check_token(tokens, i++, TokenKind::RightParen);
    check_token(tokens, i++, TokenKind::Colon);
    check_identifier(tokens, i++, "i32");
    check_token(tokens, i++, TokenKind::LeftBrace);
    check_token(tokens, i++, TokenKind::KwReturn);
    check_identifier(tokens, i++, "a");
    check_token(tokens, i++, TokenKind::Plus);
    check_identifier(tokens, i++, "b");
    check_token(tokens, i++, TokenKind::Semicolon);
    check_token(tokens, i++, TokenKind::RightBrace);
    check_token(tokens, i++, TokenKind::Eof);
}

TEST_CASE("Lexer: Code Snippet - Struct") {
    auto tokens = lex_all(
        "struct Point {\n"
        "    x: f32;\n"
        "    y: f32;\n"
        "}"
    );

    u32 i = 0;
    check_token(tokens, i++, TokenKind::KwStruct);
    check_identifier(tokens, i++, "Point");
    check_token(tokens, i++, TokenKind::LeftBrace);
    check_identifier(tokens, i++, "x");
    check_token(tokens, i++, TokenKind::Colon);
    check_identifier(tokens, i++, "f32");
    check_token(tokens, i++, TokenKind::Semicolon);
    check_identifier(tokens, i++, "y");
    check_token(tokens, i++, TokenKind::Colon);
    check_identifier(tokens, i++, "f32");
    check_token(tokens, i++, TokenKind::Semicolon);
    check_token(tokens, i++, TokenKind::RightBrace);
    check_token(tokens, i++, TokenKind::Eof);
}

TEST_CASE("Lexer: Ternary and Scope Resolution") {
    auto tokens = lex_all("x ? a : b\nFoo::bar");

    u32 i = 0;
    check_identifier(tokens, i++, "x");
    check_token(tokens, i++, TokenKind::Question);
    check_identifier(tokens, i++, "a");
    check_token(tokens, i++, TokenKind::Colon);
    check_identifier(tokens, i++, "b");
    check_identifier(tokens, i++, "Foo");
    check_token(tokens, i++, TokenKind::ColonColon);
    check_identifier(tokens, i++, "bar");
}

TEST_CASE("Lexer: Error Cases") {
    SUBCASE("Unterminated string") {
        auto tokens = lex_all("\"hello");
        check_token(tokens, 0, TokenKind::Error);
    }

    SUBCASE("Invalid character") {
        auto tokens = lex_all("foo @ bar");
        check_identifier(tokens, 0, "foo");
        check_token(tokens, 1, TokenKind::Error);
    }

    SUBCASE("Invalid hexadecimal") {
        auto tokens = lex_all("0x");
        check_token(tokens, 0, TokenKind::Error);
    }

    SUBCASE("Invalid binary") {
        auto tokens = lex_all("0b");
        check_token(tokens, 0, TokenKind::Error);
    }

    SUBCASE("Invalid octal") {
        auto tokens = lex_all("0o");
        check_token(tokens, 0, TokenKind::Error);
    }
}
