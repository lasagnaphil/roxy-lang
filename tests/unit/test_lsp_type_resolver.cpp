#include "roxy/core/doctest/doctest.h"

#include "roxy/lsp/lsp_type_resolver.hpp"
#include "roxy/lsp/cst_lowering.hpp"
#include "roxy/lsp/lsp_parser.hpp"
#include "roxy/lsp/indexer.hpp"
#include "roxy/lsp/global_index.hpp"
#include "roxy/core/bump_allocator.hpp"

#include <cstring>

using namespace rx;

// Helper to set up a GlobalIndex with given source
static void setup_index(GlobalIndex& index, const char* source, BumpAllocator& allocator) {
    u32 length = static_cast<u32>(strlen(source));
    Lexer lexer(source, length);
    LspParser parser(lexer, allocator);
    SyntaxTree tree = parser.parse();

    FileIndexer indexer;
    FileStubs stubs = indexer.index(tree.root);
    String uri("file:///test.roxy");
    index.update_file(uri, stubs);
}

// Helper to parse, lower a function, and analyze it
static void analyze_source(const char* source, LspTypeResolver& resolver,
                           BumpAllocator& parse_allocator, BumpAllocator& ast_allocator) {
    u32 length = static_cast<u32>(strlen(source));
    Lexer lexer(source, length);
    LspParser parser(lexer, parse_allocator);
    SyntaxTree tree = parser.parse();

    // Find the first function/method declaration in the CST
    SyntaxNode* fn_node = nullptr;
    for (u32 i = 0; i < tree.root->children.size(); i++) {
        SyntaxKind kind = tree.root->children[i]->kind;
        if (kind == SyntaxKind::NodeFunDecl || kind == SyntaxKind::NodeMethodDecl ||
            kind == SyntaxKind::NodeConstructorDecl || kind == SyntaxKind::NodeDestructorDecl) {
            fn_node = tree.root->children[i];
            break;
        }
    }

    if (fn_node) {
        CstLowering lowering(ast_allocator);
        Decl* ast_decl = lowering.lower_decl(fn_node);
        resolver.analyze_function(ast_decl);
    }
}

// --- GlobalIndex type info tests ---

TEST_CASE("GlobalIndex: find_struct_parent returns parent for inherited struct") {
    BumpAllocator allocator(4096);
    const char* source = "struct Base { x: i32; }\nstruct Child : Base { y: i32; }";

    GlobalIndex index;
    setup_index(index, source, allocator);

    StringView parent = index.find_struct_parent("Child");
    CHECK(parent == StringView("Base"));
}

TEST_CASE("GlobalIndex: find_struct_parent returns empty for struct without parent") {
    BumpAllocator allocator(4096);
    const char* source = "struct Orphan { x: i32; }";

    GlobalIndex index;
    setup_index(index, source, allocator);

    StringView parent = index.find_struct_parent("Orphan");
    CHECK(parent.empty());
}

TEST_CASE("GlobalIndex: find_field_type returns type") {
    BumpAllocator allocator(4096);
    const char* source = "struct Point { x: f32; y: f32; }";

    GlobalIndex index;
    setup_index(index, source, allocator);

    CHECK(index.find_field_type("Point", "x") == StringView("f32"));
    CHECK(index.find_field_type("Point", "y") == StringView("f32"));
    CHECK(index.find_field_type("Point", "z").empty());
}

TEST_CASE("GlobalIndex: find_function_return_type") {
    BumpAllocator allocator(4096);
    const char* source = "fun add(a: i32, b: i32): i32 { return a + b; }";

    GlobalIndex index;
    setup_index(index, source, allocator);

    CHECK(index.find_function_return_type("add") == StringView("i32"));
}

TEST_CASE("GlobalIndex: find_method_return_type") {
    BumpAllocator allocator(4096);
    const char* source = "fun Point.length(): f32 { return 0.0; }";

    GlobalIndex index;
    setup_index(index, source, allocator);

    CHECK(index.find_method_return_type("Point", "length") == StringView("f32"));
}

TEST_CASE("GlobalIndex: type info cleaned up on remove_file") {
    BumpAllocator allocator(4096);
    const char* source = "struct Point { x: f32; }\nfun get(): Point { }";

    GlobalIndex index;
    String uri("file:///test.roxy");
    setup_index(index, source, allocator);

    CHECK(index.find_field_type("Point", "x") == StringView("f32"));
    CHECK(index.find_function_return_type("get") == StringView("Point"));

    index.remove_file(StringView("file:///test.roxy"));

    CHECK(index.find_field_type("Point", "x").empty());
    CHECK(index.find_function_return_type("get").empty());
}

// --- LspTypeResolver tests ---

TEST_CASE("LspTypeResolver: Explicit type annotation") {
    GlobalIndex index;
    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);

    const char* source = "fun main() { var p: Point; }";
    LspTypeResolver resolver(index);
    analyze_source(source, resolver, parse_alloc, ast_alloc);

    // Parse again to get CST for resolve_cst_expr_type
    u32 length = static_cast<u32>(strlen(source));
    BumpAllocator cst_alloc(4096);
    Lexer lexer(source, length);
    LspParser parser(lexer, cst_alloc);
    SyntaxTree tree = parser.parse();

    // Find the identifier "p" in the CST
    // The var decl body: fun main() { var p: Point; }
    // We need to find NodeIdentifierExpr for "p" -- but "p" here is in a var decl, not expr
    // Let's test via an expression context instead

    // Test internal scope: "p" should resolve to "Point"
    // We can create a fake identifier node for testing
    SyntaxNode fake_ident;
    fake_ident.kind = SyntaxKind::TokenIdentifier;
    Token fake_token;
    fake_token.kind = TokenKind::Identifier;
    fake_token.start = source + 17; // "p"
    fake_token.length = 1;
    fake_ident.token = fake_token;
    fake_ident.parent = nullptr;
    fake_ident.children = Span<SyntaxNode*>();
    fake_ident.range = TextRange{17, 18};
    fake_ident.error_message = nullptr;

    String result = resolver.resolve_cst_expr_type(&fake_ident);
    CHECK(result == String("Point"));
}

TEST_CASE("LspTypeResolver: Struct literal inference") {
    BumpAllocator index_alloc(4096);
    GlobalIndex index;
    setup_index(index, "struct Point { x: f32; }", index_alloc);

    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);

    const char* source = "fun main() { var p = Point { x = 1 }; }";
    LspTypeResolver resolver(index);
    analyze_source(source, resolver, parse_alloc, ast_alloc);

    // "p" should be inferred as "Point"
    SyntaxNode fake_ident;
    fake_ident.kind = SyntaxKind::TokenIdentifier;
    Token fake_token;
    fake_token.kind = TokenKind::Identifier;
    fake_token.start = source + 17; // "p"
    fake_token.length = 1;
    fake_ident.token = fake_token;
    fake_ident.parent = nullptr;
    fake_ident.children = Span<SyntaxNode*>();
    fake_ident.range = TextRange{17, 18};
    fake_ident.error_message = nullptr;

    String result = resolver.resolve_cst_expr_type(&fake_ident);
    CHECK(result == String("Point"));
}

TEST_CASE("LspTypeResolver: Parameter type") {
    GlobalIndex index;
    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);

    const char* source = "fun foo(p: Point) { }";
    LspTypeResolver resolver(index);
    analyze_source(source, resolver, parse_alloc, ast_alloc);

    SyntaxNode fake_ident;
    fake_ident.kind = SyntaxKind::TokenIdentifier;
    Token fake_token;
    fake_token.kind = TokenKind::Identifier;
    fake_token.start = source + 8; // "p"
    fake_token.length = 1;
    fake_ident.token = fake_token;
    fake_ident.parent = nullptr;
    fake_ident.children = Span<SyntaxNode*>();
    fake_ident.range = TextRange{8, 9};
    fake_ident.error_message = nullptr;

    String result = resolver.resolve_cst_expr_type(&fake_ident);
    CHECK(result == String("Point"));
}

TEST_CASE("LspTypeResolver: Function call return type") {
    BumpAllocator index_alloc(4096);
    GlobalIndex index;
    setup_index(index, "fun get_point(): Point { }", index_alloc);

    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);

    const char* source = "fun main() { var p = get_point(); }";
    LspTypeResolver resolver(index);
    analyze_source(source, resolver, parse_alloc, ast_alloc);

    SyntaxNode fake_ident;
    fake_ident.kind = SyntaxKind::TokenIdentifier;
    Token fake_token;
    fake_token.kind = TokenKind::Identifier;
    fake_token.start = source + 17; // "p"
    fake_token.length = 1;
    fake_ident.token = fake_token;
    fake_ident.parent = nullptr;
    fake_ident.children = Span<SyntaxNode*>();
    fake_ident.range = TextRange{17, 18};
    fake_ident.error_message = nullptr;

    String result = resolver.resolve_cst_expr_type(&fake_ident);
    CHECK(result == String("Point"));
}

TEST_CASE("LspTypeResolver: Variable-to-variable") {
    GlobalIndex index;
    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);

    const char* source = "fun main() { var p: Point; var q = p; }";
    LspTypeResolver resolver(index);
    analyze_source(source, resolver, parse_alloc, ast_alloc);

    SyntaxNode fake_ident;
    fake_ident.kind = SyntaxKind::TokenIdentifier;
    Token fake_token;
    fake_token.kind = TokenKind::Identifier;
    fake_token.start = source + 31; // "q"
    fake_token.length = 1;
    fake_ident.token = fake_token;
    fake_ident.parent = nullptr;
    fake_ident.children = Span<SyntaxNode*>();
    fake_ident.range = TextRange{31, 32};
    fake_ident.error_message = nullptr;

    String result = resolver.resolve_cst_expr_type(&fake_ident);
    CHECK(result == String("Point"));
}

TEST_CASE("LspTypeResolver: Self type in method") {
    BumpAllocator index_alloc(4096);
    GlobalIndex index;
    setup_index(index, "struct Point { x: f32; }", index_alloc);

    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);

    const char* source = "fun Point.get_x(): f32 { return self.x; }";
    LspTypeResolver resolver(index);
    analyze_source(source, resolver, parse_alloc, ast_alloc);

    // "self" should resolve to "Point"
    SyntaxNode fake_self;
    fake_self.kind = SyntaxKind::TokenKwSelf;
    fake_self.parent = nullptr;
    fake_self.children = Span<SyntaxNode*>();
    fake_self.range = TextRange{32, 36};
    fake_self.error_message = nullptr;

    String result = resolver.resolve_cst_expr_type(&fake_self);
    CHECK(result == String("Point"));
}

TEST_CASE("LspTypeResolver: Chained field access") {
    BumpAllocator index_alloc(4096);
    GlobalIndex index;
    const char* types_source =
        "struct Inner { value: i32; }\n"
        "struct Outer { inner: Inner; }";
    setup_index(index, types_source, index_alloc);

    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);

    const char* source = "fun main() { var o: Outer; }";
    LspTypeResolver resolver(index);
    analyze_source(source, resolver, parse_alloc, ast_alloc);

    // Resolve "o" -> "Outer"
    SyntaxNode fake_ident;
    fake_ident.kind = SyntaxKind::TokenIdentifier;
    Token fake_token;
    fake_token.kind = TokenKind::Identifier;
    fake_token.start = source + 17; // "o"
    fake_token.length = 1;
    fake_ident.token = fake_token;
    fake_ident.parent = nullptr;
    fake_ident.children = Span<SyntaxNode*>();
    fake_ident.range = TextRange{17, 18};
    fake_ident.error_message = nullptr;

    String result = resolver.resolve_cst_expr_type(&fake_ident);
    CHECK(result == String("Outer"));

    // Test field type in hierarchy
    CHECK(index.find_field_type("Outer", "inner") == StringView("Inner"));
    CHECK(index.find_field_type("Inner", "value") == StringView("i32"));
}

TEST_CASE("LspTypeResolver: Unresolvable returns empty") {
    GlobalIndex index;
    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);

    const char* source = "fun main() { var p = ???; }";
    LspTypeResolver resolver(index);
    analyze_source(source, resolver, parse_alloc, ast_alloc);

    SyntaxNode fake_ident;
    fake_ident.kind = SyntaxKind::TokenIdentifier;
    Token fake_token;
    fake_token.kind = TokenKind::Identifier;
    // "p" may or may not be resolvable depending on error recovery,
    // but "unknown" should definitely not be resolvable
    const char* unknown_name = "unknown";
    fake_token.start = unknown_name;
    fake_token.length = 7;
    fake_ident.token = fake_token;
    fake_ident.parent = nullptr;
    fake_ident.children = Span<SyntaxNode*>();
    fake_ident.range = TextRange{0, 7};
    fake_ident.error_message = nullptr;

    String result = resolver.resolve_cst_expr_type(&fake_ident);
    CHECK(result.empty());
}

// --- Field-access go-to-definition integration-style tests ---
// These test the GlobalIndex field/method resolution through the hierarchy

TEST_CASE("LspTypeResolver: Inherited field resolution") {
    BumpAllocator index_alloc(4096);
    GlobalIndex index;
    const char* source = "struct Base { x: i32; }\nstruct Child : Base { y: i32; }";
    setup_index(index, source, index_alloc);

    // Direct field
    CHECK(index.find_field("Child", "y") != nullptr);
    // Inherited field should not be in find_field (it's on Base)
    CHECK(index.find_field("Child", "x") == nullptr);
    // But we can walk up the hierarchy
    CHECK(index.find_field("Base", "x") != nullptr);
    CHECK(index.find_struct_parent("Child") == StringView("Base"));
}
