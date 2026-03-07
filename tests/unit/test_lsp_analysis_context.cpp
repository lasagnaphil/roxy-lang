#include "roxy/core/doctest/doctest.h"

#include "roxy/lsp/lsp_analysis_context.hpp"
#include "roxy/lsp/lsp_parser.hpp"
#include "roxy/lsp/indexer.hpp"
#include "roxy/core/bump_allocator.hpp"

#include <cstring>

using namespace rx;

// Helper to parse source and create a SourceFile for rebuild_declarations
static LspAnalysisContext::SourceFile make_source_file(
    const char* source, BumpAllocator& allocator, SyntaxTree& out_tree)
{
    u32 length = static_cast<u32>(strlen(source));
    Lexer lexer(source, length);
    LspParser parser(lexer, allocator);
    out_tree = parser.parse();

    LspAnalysisContext::SourceFile file;
    file.uri = StringView("file:///test.roxy");
    file.source = source;
    file.source_length = length;
    file.cst_root = out_tree.root;
    return file;
}

// Find the first function/method node in a CST
static SyntaxNode* find_first_function(SyntaxNode* root) {
    if (!root) return nullptr;
    for (u32 i = 0; i < root->children.size(); i++) {
        SyntaxKind kind = root->children[i]->kind;
        if (kind == SyntaxKind::NodeFunDecl || kind == SyntaxKind::NodeMethodDecl ||
            kind == SyntaxKind::NodeConstructorDecl || kind == SyntaxKind::NodeDestructorDecl) {
            return root->children[i];
        }
    }
    return nullptr;
}

TEST_CASE("LSP Analysis Context - initialization") {
    LspAnalysisContext context;
    CHECK(!context.is_initialized());
    CHECK(context.declaration_version() == 0);
}

TEST_CASE("LSP Analysis Context - rebuild declarations simple") {
    LspAnalysisContext context;

    const char* source = "fun add(a: i32, b: i32): i32 { return a + b; }";

    BumpAllocator allocator(8192);
    SyntaxTree tree;
    auto file = make_source_file(source, allocator, tree);

    Span<LspAnalysisContext::SourceFile> files(&file, 1);
    context.rebuild_declarations(files);

    CHECK(context.is_initialized());
    CHECK(context.declaration_version() == 1);
}

TEST_CASE("LSP Analysis Context - rebuild declarations") {
    LspAnalysisContext context;

    const char* source = R"(
struct Point {
    x: i32;
    y: i32;
}

fun add(a: i32, b: i32): i32 {
    return a + b;
}
)";

    BumpAllocator allocator(8192);
    SyntaxTree tree;
    auto file = make_source_file(source, allocator, tree);

    Span<LspAnalysisContext::SourceFile> files(&file, 1);
    context.rebuild_declarations(files);

    CHECK(context.is_initialized());
    CHECK(context.declaration_version() == 1);

    // Type should be registered
    Type* point_type = context.type_env().type_by_name(StringView("Point"));
    CHECK(point_type != nullptr);
    if (point_type) {
        CHECK(point_type->kind == TypeKind::Struct);
        CHECK(point_type->struct_info.name == StringView("Point"));
    }
}

TEST_CASE("LSP Analysis Context - analyze function body") {
    LspAnalysisContext context;

    const char* source = R"(
struct Point {
    x: i32;
    y: i32;
}

fun test(): i32 {
    var p = Point { x = 10, y = 20 };
    return p.x + p.y;
}
)";

    BumpAllocator parse_allocator(8192);
    SyntaxTree tree;
    auto file = make_source_file(source, parse_allocator, tree);

    Span<LspAnalysisContext::SourceFile> files(&file, 1);
    context.rebuild_declarations(files);

    CHECK(context.is_initialized());

    // Find the function node
    SyntaxNode* fn_node = find_first_function(tree.root);
    REQUIRE(fn_node != nullptr);

    // Analyze the function body
    BumpAllocator ast_allocator(8192);
    BodyAnalysisResult result = context.analyze_function_body(fn_node, ast_allocator);

    CHECK(result.decl != nullptr);
    CHECK(result.symbols != nullptr);
    // Body analysis may have errors since the function is being analyzed
    // in isolation — that's expected

    delete result.symbols;
}

TEST_CASE("LSP Analysis Context - type_to_string") {
    LspAnalysisContext context;

    const char* source = R"(
struct Point { x: i32; y: i32; }
)";

    BumpAllocator allocator(8192);
    SyntaxTree tree;
    auto file = make_source_file(source, allocator, tree);

    Span<LspAnalysisContext::SourceFile> files(&file, 1);
    context.rebuild_declarations(files);

    // Primitive types
    CHECK(LspAnalysisContext::type_to_string(context.types().i32_type()) == String("i32"));
    CHECK(LspAnalysisContext::type_to_string(context.types().f64_type()) == String("f64"));
    CHECK(LspAnalysisContext::type_to_string(context.types().bool_type()) == String("bool"));
    CHECK(LspAnalysisContext::type_to_string(context.types().string_type()) == String("string"));
    CHECK(LspAnalysisContext::type_to_string(context.types().void_type()) == String("void"));

    // Struct type
    Type* point_type = context.type_env().type_by_name(StringView("Point"));
    REQUIRE(point_type != nullptr);
    CHECK(LspAnalysisContext::type_to_string(point_type) == String("Point"));

    // List type
    Type* list_type = context.types().list_type(context.types().i32_type());
    CHECK(LspAnalysisContext::type_to_string(list_type) == String("List<i32>"));

    // Map type
    Type* map_type = context.types().map_type(
        context.types().string_type(), context.types().i32_type());
    CHECK(LspAnalysisContext::type_to_string(map_type) == String("Map<string, i32>"));

    // Null type
    CHECK(LspAnalysisContext::type_to_string(nullptr) == String("unknown"));
}

TEST_CASE("LSP Analysis Context - rebuild increments version") {
    LspAnalysisContext context;

    const char* source1 = "fun test(): i32 { return 1; }";

    BumpAllocator allocator1(8192);
    SyntaxTree tree1;
    auto file1 = make_source_file(source1, allocator1, tree1);

    Span<LspAnalysisContext::SourceFile> files1(&file1, 1);
    context.rebuild_declarations(files1);
    CHECK(context.declaration_version() == 1);

    const char* source2 = R"(
struct Foo { x: i32; }
fun test(): i32 { return 1; }
)";

    BumpAllocator allocator2(8192);
    SyntaxTree tree2;
    auto file2 = make_source_file(source2, allocator2, tree2);

    Span<LspAnalysisContext::SourceFile> files2(&file2, 1);
    context.rebuild_declarations(files2);
    CHECK(context.declaration_version() == 2);

    // New struct should be available
    Type* foo_type = context.type_env().type_by_name(StringView("Foo"));
    CHECK(foo_type != nullptr);
}
