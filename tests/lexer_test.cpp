#include "roxy/shared/lexer.hpp"

#include <cstdio>
#include <cstring>

using namespace rx;

void print_token(const Token& token) {
    printf("  [%u:%u] %-15s ", token.loc.line, token.loc.column, token_kind_to_string(token.kind));

    if (token.kind == TokenKind::IntLiteral) {
        printf("value=%lld ", (long long)token.int_value);
    } else if (token.kind == TokenKind::FloatLiteral) {
        printf("value=%f ", token.float_value);
    }

    if (token.kind != TokenKind::Error && token.kind != TokenKind::Eof) {
        printf("'%.*s'", token.length, token.start);
    } else if (token.kind == TokenKind::Error) {
        printf("error: %.*s", token.length, token.start);
    }

    printf("\n");
}

void test_lexer(const char* name, const char* source) {
    printf("=== %s ===\n", name);
    printf("Input: %s\n", source);
    printf("Tokens:\n");

    Lexer lexer(source, (u32)strlen(source));
    while (!lexer.is_at_end()) {
        Token token = lexer.next_token();
        print_token(token);
        if (token.kind == TokenKind::Eof || token.kind == TokenKind::Error) {
            break;
        }
    }
    printf("\n");
}

int main() {
    // Test keywords
    test_lexer("Keywords",
        "true false nil var fun struct enum pub native "
        "if else for while break continue return when case "
        "this super new delete "
        "uniq ref weak out inout "
        "import from");

    // Test identifiers
    test_lexer("Identifiers",
        "foo _bar baz123 _123 camelCase snake_case PascalCase");

    // Test integer literals
    test_lexer("Integer Literals",
        "0 123 456u 789l 1000ul 1000UL");

    // Test hex, binary, octal
    test_lexer("Number Bases",
        "0xFF 0xff 0b1010 0B1111 0o77 0O755");

    // Test float literals
    test_lexer("Float Literals",
        "1.5 3.14159 0.5 1.0f 2.5F");

    // Test string literals
    test_lexer("String Literals",
        "\"hello\" \"world\" \"with\\nescapes\" \"tab\\there\"");

    // Test operators (single char)
    test_lexer("Single Char Operators",
        "( ) { } [ ] , . ; : ? ~");

    // Test operators (one or two char)
    test_lexer("Multi Char Operators",
        "+ += - -= * *= / /= % %= "
        "! != = == < <= > >= "
        "& && | || ::");

    // Test comments
    test_lexer("Comments",
        "foo // line comment\n"
        "bar /* block comment */ baz\n"
        "/* nested /* comment */ */ qux");

    // Test full code snippet
    test_lexer("Code Snippet",
        "fun add(a: i32, b: i32): i32 {\n"
        "    return a + b;\n"
        "}\n"
        "\n"
        "var x = 10;\n"
        "var y = add(x, 5);\n");

    // Test struct declaration
    test_lexer("Struct Declaration",
        "struct Point {\n"
        "    x: f32;\n"
        "    y: f32;\n"
        "}\n");

    // Test control flow
    test_lexer("Control Flow",
        "if (x > 0) {\n"
        "    while (x > 0) {\n"
        "        x = x - 1;\n"
        "    }\n"
        "} else {\n"
        "    return nil;\n"
        "}\n");

    // Test ternary and scope resolution
    test_lexer("Ternary and Scope",
        "x ? a : b\n"
        "Foo::bar\n"
        "obj.method()\n");

    // Test error cases
    test_lexer("Unterminated String",
        "\"hello");

    test_lexer("Invalid Character",
        "foo @ bar");

    test_lexer("Invalid Hex",
        "0x");

    printf("=== All tests completed ===\n");
    return 0;
}
