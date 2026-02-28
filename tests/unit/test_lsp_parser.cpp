#include "roxy/core/doctest/doctest.h"

#include "roxy/lsp/lsp_parser.hpp"
#include "roxy/core/bump_allocator.hpp"

#include <cstring>

using namespace rx;

// Helper to parse source and return SyntaxTree
static SyntaxTree parse_source(const char* source, BumpAllocator& allocator) {
    u32 length = static_cast<u32>(strlen(source));
    Lexer lexer(source, length);
    LspParser parser(lexer, allocator);
    return parser.parse();
}

// Helper to find first child of a given kind
static SyntaxNode* find_child(SyntaxNode* node, SyntaxKind kind) {
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == kind) return node->children[i];
    }
    return nullptr;
}

// Helper to count children of a given kind
static u32 count_children(SyntaxNode* node, SyntaxKind kind) {
    u32 count = 0;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == kind) count++;
    }
    return count;
}

// Helper to check if any node in the tree is an Error node
static bool has_error_node(SyntaxNode* node) {
    if (node->kind == SyntaxKind::Error) return true;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (has_error_node(node->children[i])) return true;
    }
    return false;
}

TEST_CASE("LSP Parser: Clean source produces no diagnostics") {
    BumpAllocator allocator(4096);

    SUBCASE("Empty source") {
        SyntaxTree tree = parse_source("", allocator);
        CHECK(tree.root != nullptr);
        CHECK(tree.root->kind == SyntaxKind::NodeProgram);
        CHECK(tree.diagnostics.empty());
    }

    SUBCASE("Simple variable declaration") {
        SyntaxTree tree = parse_source("var x: i32 = 42;", allocator);
        CHECK(tree.root != nullptr);
        CHECK(tree.root->kind == SyntaxKind::NodeProgram);
        CHECK(tree.diagnostics.empty());
        CHECK(tree.root->children.size() == 1);
        CHECK(tree.root->children[0]->kind == SyntaxKind::NodeVarDecl);
    }

    SUBCASE("Simple function declaration") {
        SyntaxTree tree = parse_source("fun add(a: i32, b: i32): i32 { return a + b; }", allocator);
        CHECK(tree.root != nullptr);
        CHECK(tree.diagnostics.empty());
        CHECK(tree.root->children.size() == 1);
        CHECK(tree.root->children[0]->kind == SyntaxKind::NodeFunDecl);
    }

    SUBCASE("Struct declaration") {
        SyntaxTree tree = parse_source("struct Point { x: f32; y: f32; }", allocator);
        CHECK(tree.root != nullptr);
        CHECK(tree.diagnostics.empty());
        CHECK(tree.root->children.size() == 1);
        CHECK(tree.root->children[0]->kind == SyntaxKind::NodeStructDecl);
    }
}

TEST_CASE("LSP Parser: Missing semicolon recovery") {
    BumpAllocator allocator(4096);

    SUBCASE("Missing semicolon after var decl") {
        SyntaxTree tree = parse_source("var x: i32 = 42\nvar y: i32 = 10;", allocator);
        CHECK(tree.root != nullptr);
        CHECK(tree.root->kind == SyntaxKind::NodeProgram);
        // Should still produce a tree with diagnostics
        CHECK(!tree.diagnostics.empty());
        // Both declarations should be present
        CHECK(tree.root->children.size() >= 1);
    }

    SUBCASE("Missing semicolon after expression") {
        SyntaxTree tree = parse_source("x + 1\nvar y = 2;", allocator);
        CHECK(tree.root != nullptr);
        CHECK(!tree.diagnostics.empty());
    }
}

TEST_CASE("LSP Parser: Missing closing brace") {
    BumpAllocator allocator(4096);

    SUBCASE("Missing closing brace on function") {
        SyntaxTree tree = parse_source("fun foo() { var x = 1;", allocator);
        CHECK(tree.root != nullptr);
        CHECK(!tree.diagnostics.empty());
        // Tree should still have a function declaration
        CHECK(tree.root->children.size() >= 1);
    }

    SUBCASE("Missing closing brace on struct") {
        SyntaxTree tree = parse_source("struct Point { x: f32;", allocator);
        CHECK(tree.root != nullptr);
        CHECK(!tree.diagnostics.empty());
    }
}

TEST_CASE("LSP Parser: Multiple errors collected") {
    BumpAllocator allocator(4096);

    SUBCASE("Multiple missing semicolons") {
        SyntaxTree tree = parse_source("var x = 1\nvar y = 2\nvar z = 3;", allocator);
        CHECK(tree.root != nullptr);
        // Should collect multiple diagnostics (at least 2 for the first two missing semicolons)
        CHECK(tree.diagnostics.size() >= 2);
    }
}

TEST_CASE("LSP Parser: CST structure for declarations") {
    BumpAllocator allocator(4096);

    SUBCASE("Var declaration children") {
        SyntaxTree tree = parse_source("var x: i32 = 42;", allocator);
        REQUIRE(tree.root->children.size() == 1);
        SyntaxNode* var_decl = tree.root->children[0];
        CHECK(var_decl->kind == SyntaxKind::NodeVarDecl);
        // Should have children: name, ':', type, '=', init_expr, ';'
        CHECK(var_decl->children.size() >= 4);
    }

    SUBCASE("Fun declaration children") {
        SyntaxTree tree = parse_source("fun foo(x: i32): i32 { return x; }", allocator);
        REQUIRE(tree.root->children.size() == 1);
        SyntaxNode* fun_decl = tree.root->children[0];
        CHECK(fun_decl->kind == SyntaxKind::NodeFunDecl);
        // Should have: name, '(', params, ')', ':', return_type, '{', block
        CHECK(fun_decl->children.size() >= 6);
    }

    SUBCASE("Struct declaration with fields") {
        SyntaxTree tree = parse_source("struct Vec2 { x: f32; y: f32; }", allocator);
        REQUIRE(tree.root->children.size() == 1);
        SyntaxNode* struct_decl = tree.root->children[0];
        CHECK(struct_decl->kind == SyntaxKind::NodeStructDecl);
        // Should have: name, '{', field1, field2, '}'
        u32 field_count = count_children(struct_decl, SyntaxKind::NodeFieldDecl);
        CHECK(field_count == 2);
    }
}

TEST_CASE("LSP Parser: Recovery inside struct body") {
    BumpAllocator allocator(4096);

    SUBCASE("Bad field followed by good field") {
        SyntaxTree tree = parse_source("struct Foo { 123; y: f32; }", allocator);
        CHECK(tree.root != nullptr);
        CHECK(!tree.diagnostics.empty());
        // The struct should still be parsed
        SyntaxNode* struct_decl = tree.root->children[0];
        CHECK(struct_decl->kind == SyntaxKind::NodeStructDecl);
    }
}

TEST_CASE("LSP Parser: Expression parsing") {
    BumpAllocator allocator(4096);

    SUBCASE("Binary expression") {
        SyntaxTree tree = parse_source("1 + 2;", allocator);
        CHECK(tree.root != nullptr);
        CHECK(tree.diagnostics.empty());
        // ExprStmt -> BinaryExpr
        SyntaxNode* expr_stmt = tree.root->children[0];
        CHECK(expr_stmt->kind == SyntaxKind::NodeExprStmt);
    }

    SUBCASE("Unary expression") {
        SyntaxTree tree = parse_source("-x;", allocator);
        CHECK(tree.root != nullptr);
        CHECK(tree.diagnostics.empty());
    }

    SUBCASE("Call expression") {
        SyntaxTree tree = parse_source("foo(1, 2);", allocator);
        CHECK(tree.root != nullptr);
        CHECK(tree.diagnostics.empty());
    }

    SUBCASE("Index expression") {
        SyntaxTree tree = parse_source("arr[0];", allocator);
        CHECK(tree.root != nullptr);
        CHECK(tree.diagnostics.empty());
    }

    SUBCASE("Member access") {
        SyntaxTree tree = parse_source("obj.field;", allocator);
        CHECK(tree.root != nullptr);
        CHECK(tree.diagnostics.empty());
    }
}

TEST_CASE("LSP Parser: Incomplete expression") {
    BumpAllocator allocator(4096);

    SUBCASE("var x = followed by nothing") {
        SyntaxTree tree = parse_source("var x = ;", allocator);
        CHECK(tree.root != nullptr);
        // Should have an error where expression is expected
        CHECK(!tree.diagnostics.empty());
        // But tree structure should still exist
        CHECK(tree.root->children.size() >= 1);
    }

    SUBCASE("Unclosed parenthesis") {
        SyntaxTree tree = parse_source("(1 + 2;", allocator);
        CHECK(tree.root != nullptr);
        CHECK(!tree.diagnostics.empty());
    }
}

TEST_CASE("LSP Parser: Control flow statements") {
    BumpAllocator allocator(4096);

    SUBCASE("If statement") {
        SyntaxTree tree = parse_source("if (x > 0) { return x; }", allocator);
        CHECK(tree.root != nullptr);
        CHECK(tree.diagnostics.empty());
    }

    SUBCASE("While statement") {
        SyntaxTree tree = parse_source("while (i < 10) { i = i + 1; }", allocator);
        CHECK(tree.root != nullptr);
        CHECK(tree.diagnostics.empty());
    }

    SUBCASE("For statement") {
        SyntaxTree tree = parse_source("for (var i = 0; i < 10; i = i + 1) { x = x + i; }", allocator);
        CHECK(tree.root != nullptr);
        CHECK(tree.diagnostics.empty());
    }
}

TEST_CASE("LSP Parser: Enum declaration") {
    BumpAllocator allocator(4096);

    SyntaxTree tree = parse_source("enum Color { Red, Green, Blue }", allocator);
    CHECK(tree.root != nullptr);
    CHECK(tree.diagnostics.empty());
    REQUIRE(tree.root->children.size() == 1);
    CHECK(tree.root->children[0]->kind == SyntaxKind::NodeEnumDecl);
}

TEST_CASE("LSP Parser: Trait declaration") {
    BumpAllocator allocator(4096);

    SyntaxTree tree = parse_source("trait Printable;", allocator);
    CHECK(tree.root != nullptr);
    CHECK(tree.diagnostics.empty());
    REQUIRE(tree.root->children.size() == 1);
    CHECK(tree.root->children[0]->kind == SyntaxKind::NodeTraitDecl);
}

TEST_CASE("LSP Parser: Import declaration") {
    BumpAllocator allocator(4096);

    SUBCASE("Simple import") {
        SyntaxTree tree = parse_source("import math;", allocator);
        CHECK(tree.root != nullptr);
        CHECK(tree.diagnostics.empty());
        CHECK(tree.root->children[0]->kind == SyntaxKind::NodeImportDecl);
    }

    SUBCASE("From import") {
        SyntaxTree tree = parse_source("from math import sin, cos;", allocator);
        CHECK(tree.root != nullptr);
        CHECK(tree.diagnostics.empty());
        CHECK(tree.root->children[0]->kind == SyntaxKind::NodeImportDecl);
    }
}

TEST_CASE("LSP Parser: Try/catch/throw") {
    BumpAllocator allocator(4096);

    SyntaxTree tree = parse_source(
        "try { throw x; } catch (e: MyError) { var y = 1; } finally { var z = 2; }",
        allocator);
    CHECK(tree.root != nullptr);
    CHECK(tree.diagnostics.empty());
}

TEST_CASE("LSP Parser: Method declaration") {
    BumpAllocator allocator(4096);

    SyntaxTree tree = parse_source("fun Vec2.len(): f32 { return 0.0; }", allocator);
    CHECK(tree.root != nullptr);
    CHECK(tree.diagnostics.empty());
    REQUIRE(tree.root->children.size() == 1);
    CHECK(tree.root->children[0]->kind == SyntaxKind::NodeMethodDecl);
}

TEST_CASE("LSP Parser: Source range tracking") {
    BumpAllocator allocator(4096);

    SyntaxTree tree = parse_source("var x = 1;", allocator);
    REQUIRE(tree.root != nullptr);
    REQUIRE(tree.root->children.size() == 1);

    SyntaxNode* var_decl = tree.root->children[0];
    CHECK(var_decl->range.start == 0);
    CHECK(var_decl->range.end == 10);
}
