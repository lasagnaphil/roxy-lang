#include "roxy/core/doctest/doctest.h"

#include "roxy/lsp/global_index.hpp"
#include "roxy/lsp/lsp_type_resolver.hpp"
#include "roxy/lsp/cst_lowering.hpp"
#include "roxy/lsp/lsp_parser.hpp"
#include "roxy/lsp/indexer.hpp"
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

// --- GlobalIndex enumeration tests ---

TEST_CASE("Completion - GlobalIndex: get_struct_fields returns field names") {
    BumpAllocator allocator(4096);
    GlobalIndex index;
    setup_index(index, "struct Point { x: f32; y: f32; }", allocator);

    const Vector<String>* fields = index.get_struct_fields("Point");
    REQUIRE(fields != nullptr);
    CHECK(fields->size() == 2);
    CHECK((*fields)[0] == String("x"));
    CHECK((*fields)[1] == String("y"));
}

TEST_CASE("Completion - GlobalIndex: get_struct_fields returns nullptr for unknown struct") {
    BumpAllocator allocator(4096);
    GlobalIndex index;
    setup_index(index, "struct Point { x: f32; }", allocator);

    CHECK(index.get_struct_fields("Unknown") == nullptr);
}

TEST_CASE("Completion - GlobalIndex: get_struct_methods returns method names") {
    BumpAllocator allocator(4096);
    GlobalIndex index;
    setup_index(index,
        "struct Point { x: f32; }\n"
        "fun Point.length(): f32 { return 0.0; }\n"
        "fun Point.sum(): f32 { return 0.0; }",
        allocator);

    const Vector<String>* methods = index.get_struct_methods("Point");
    REQUIRE(methods != nullptr);
    CHECK(methods->size() == 2);
    CHECK((*methods)[0] == String("length"));
    CHECK((*methods)[1] == String("sum"));
}

TEST_CASE("Completion - GlobalIndex: get_enum_variants returns variant names") {
    BumpAllocator allocator(4096);
    GlobalIndex index;
    setup_index(index, "enum Color { Red, Green, Blue }", allocator);

    const Vector<String>* variants = index.get_enum_variants("Color");
    REQUIRE(variants != nullptr);
    CHECK(variants->size() == 3);
    CHECK((*variants)[0] == String("Red"));
    CHECK((*variants)[1] == String("Green"));
    CHECK((*variants)[2] == String("Blue"));
}

TEST_CASE("Completion - GlobalIndex: find_function_signature") {
    BumpAllocator allocator(4096);
    GlobalIndex index;
    setup_index(index, "fun add(a: i32, b: i32): i32 { return a + b; }", allocator);

    StringView sig = index.find_function_signature("add");
    CHECK(sig == StringView("(a: i32, b: i32): i32"));
}

TEST_CASE("Completion - GlobalIndex: find_function_signature void return") {
    BumpAllocator allocator(4096);
    GlobalIndex index;
    setup_index(index, "fun greet(name: string) { }", allocator);

    StringView sig = index.find_function_signature("greet");
    CHECK(sig == StringView("(name: string)"));
}

TEST_CASE("Completion - GlobalIndex: find_method_signature") {
    BumpAllocator allocator(4096);
    GlobalIndex index;
    setup_index(index,
        "struct Point { x: f32; }\n"
        "fun Point.length(): f32 { return 0.0; }",
        allocator);

    StringView sig = index.find_method_signature("Point", "length");
    CHECK(sig == StringView("(): f32"));
}

TEST_CASE("Completion - GlobalIndex: for_each_struct iterates all structs") {
    BumpAllocator allocator(4096);
    GlobalIndex index;
    setup_index(index,
        "struct Point { x: f32; }\n"
        "struct Line { a: Point; }",
        allocator);

    Vector<String> names;
    index.for_each_struct([&](const String& name) {
        names.push_back(name);
    });
    CHECK(names.size() == 2);
    // Order is unspecified (hash map), so check both present
    bool has_point = false, has_line = false;
    for (u32 i = 0; i < names.size(); i++) {
        if (names[i] == String("Point")) has_point = true;
        if (names[i] == String("Line")) has_line = true;
    }
    CHECK(has_point);
    CHECK(has_line);
}

TEST_CASE("Completion - GlobalIndex: cleanup on remove_file") {
    BumpAllocator allocator(4096);
    GlobalIndex index;
    setup_index(index,
        "struct Point { x: f32; }\n"
        "fun Point.length(): f32 { return 0.0; }\n"
        "enum Color { Red, Green }\n"
        "fun add(a: i32): i32 { return a; }",
        allocator);

    CHECK(index.get_struct_fields("Point") != nullptr);
    CHECK(index.get_struct_methods("Point") != nullptr);
    CHECK(index.get_enum_variants("Color") != nullptr);
    CHECK(!index.find_function_signature("add").empty());
    CHECK(!index.find_method_signature("Point", "length").empty());

    index.remove_file(StringView("file:///test.roxy"));

    CHECK(index.get_struct_fields("Point") == nullptr);
    CHECK(index.get_struct_methods("Point") == nullptr);
    CHECK(index.get_enum_variants("Color") == nullptr);
    CHECK(index.find_function_signature("add").empty());
    CHECK(index.find_method_signature("Point", "length").empty());
}

// --- LspTypeResolver accessor tests ---

TEST_CASE("Completion - LspTypeResolver: var_types accessible") {
    GlobalIndex index;
    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);

    const char* source = "fun main() { var p: Point; var q: i32; }";
    u32 length = static_cast<u32>(strlen(source));

    LspTypeResolver resolver(index);

    Lexer lexer(source, length);
    LspParser parser(lexer, parse_alloc);
    SyntaxTree tree = parser.parse();

    SyntaxNode* fn_node = nullptr;
    for (u32 i = 0; i < tree.root->children.size(); i++) {
        if (tree.root->children[i]->kind == SyntaxKind::NodeFunDecl) {
            fn_node = tree.root->children[i];
            break;
        }
    }
    REQUIRE(fn_node != nullptr);

    CstLowering lowering(ast_alloc);
    Decl* ast_decl = lowering.lower_decl(fn_node);
    resolver.analyze_function(ast_decl);

    const auto& var_types = resolver.var_types();
    CHECK(var_types.size() == 2);

    auto p_it = var_types.find(String("p"));
    REQUIRE(p_it != var_types.end());
    CHECK(p_it->second == String("Point"));

    auto q_it = var_types.find(String("q"));
    REQUIRE(q_it != var_types.end());
    CHECK(q_it->second == String("i32"));
}

TEST_CASE("Completion - LspTypeResolver: self_type in method") {
    BumpAllocator index_alloc(4096);
    GlobalIndex index;
    setup_index(index, "struct Point { x: f32; }", index_alloc);

    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);

    const char* source = "fun Point.get_x(): f32 { return self.x; }";
    u32 length = static_cast<u32>(strlen(source));

    LspTypeResolver resolver(index);

    Lexer lexer(source, length);
    LspParser parser(lexer, parse_alloc);
    SyntaxTree tree = parser.parse();

    SyntaxNode* fn_node = nullptr;
    for (u32 i = 0; i < tree.root->children.size(); i++) {
        if (tree.root->children[i]->kind == SyntaxKind::NodeMethodDecl) {
            fn_node = tree.root->children[i];
            break;
        }
    }
    REQUIRE(fn_node != nullptr);

    CstLowering lowering(ast_alloc);
    Decl* ast_decl = lowering.lower_decl(fn_node);
    resolver.analyze_function(ast_decl);

    CHECK(resolver.self_type() == String("Point"));
}

// --- Completion context detection tests ---
// These test the context detection logic indirectly through integration

TEST_CASE("Completion - Integration: dot access completes struct fields and methods") {
    BumpAllocator allocator(4096);
    GlobalIndex index;
    setup_index(index,
        "struct Point { x: f32; y: f32; }\n"
        "fun Point.length(): f32 { return 0.0; }",
        allocator);

    // Source with cursor after "p."
    const char* source = "fun main() { var p: Point; p. }";
    u32 length = static_cast<u32>(strlen(source));

    // Re-parse to get CST
    BumpAllocator parse_alloc(8192);
    Lexer lexer(source, length);
    LspParser parser(lexer, parse_alloc);
    SyntaxTree tree = parser.parse();

    // Find enclosing function and resolve types
    SyntaxNode* fn_node = nullptr;
    for (u32 i = 0; i < tree.root->children.size(); i++) {
        if (tree.root->children[i]->kind == SyntaxKind::NodeFunDecl) {
            fn_node = tree.root->children[i];
            break;
        }
    }
    REQUIRE(fn_node != nullptr);

    BumpAllocator ast_alloc(8192);
    CstLowering lowering(ast_alloc);
    Decl* ast_decl = lowering.lower_decl(fn_node);

    LspTypeResolver resolver(index);
    resolver.analyze_function(ast_decl);

    // Verify "p" resolves to Point
    auto p_it = resolver.var_types().find(String("p"));
    REQUIRE(p_it != resolver.var_types().end());
    CHECK(p_it->second == String("Point"));

    // Verify fields and methods are available
    const Vector<String>* fields = index.get_struct_fields("Point");
    REQUIRE(fields != nullptr);
    CHECK(fields->size() == 2);

    const Vector<String>* methods = index.get_struct_methods("Point");
    REQUIRE(methods != nullptr);
    CHECK(methods->size() == 1);
    CHECK((*methods)[0] == String("length"));
}

TEST_CASE("Completion - Integration: inherited fields in dot completion") {
    BumpAllocator allocator(4096);
    GlobalIndex index;
    setup_index(index,
        "struct Base { x: i32; }\n"
        "struct Child : Base { y: i32; }",
        allocator);

    // Walk parent chain: Child has y, Base has x
    const Vector<String>* child_fields = index.get_struct_fields("Child");
    REQUIRE(child_fields != nullptr);
    CHECK(child_fields->size() == 1);
    CHECK((*child_fields)[0] == String("y"));

    // Parent fields
    StringView parent = index.find_struct_parent("Child");
    CHECK(parent == StringView("Base"));

    const Vector<String>* base_fields = index.get_struct_fields(parent);
    REQUIRE(base_fields != nullptr);
    CHECK(base_fields->size() == 1);
    CHECK((*base_fields)[0] == String("x"));
}

TEST_CASE("Completion - Integration: enum variant completion after ::") {
    BumpAllocator allocator(4096);
    GlobalIndex index;
    setup_index(index, "enum Color { Red, Green, Blue }", allocator);

    const Vector<String>* variants = index.get_enum_variants("Color");
    REQUIRE(variants != nullptr);
    CHECK(variants->size() == 3);
}

TEST_CASE("Completion - Integration: type annotation includes primitives and user types") {
    BumpAllocator allocator(4096);
    GlobalIndex index;
    setup_index(index,
        "struct Point { x: f32; }\n"
        "enum Color { Red }\n"
        "trait Printable { }",
        allocator);

    // Verify all user types are enumerable
    Vector<String> struct_names;
    index.for_each_struct([&](const String& name) {
        struct_names.push_back(name);
    });
    CHECK(struct_names.size() == 1);
    CHECK(struct_names[0] == String("Point"));

    Vector<String> enum_names;
    index.for_each_enum([&](const String& name) {
        enum_names.push_back(name);
    });
    CHECK(enum_names.size() == 1);
    CHECK(enum_names[0] == String("Color"));

    Vector<String> trait_names;
    index.for_each_trait([&](const String& name) {
        trait_names.push_back(name);
    });
    CHECK(trait_names.size() == 1);
    CHECK(trait_names[0] == String("Printable"));
}

TEST_CASE("Completion - Integration: bare identifier includes locals, globals, functions, keywords") {
    BumpAllocator index_alloc(4096);
    GlobalIndex index;
    setup_index(index,
        "var global_count: i32;\n"
        "fun helper(): i32 { return 0; }\n"
        "struct Point { x: f32; }",
        index_alloc);

    // Verify globals enumerable
    Vector<String> global_names;
    index.for_each_global([&](const String& name) {
        global_names.push_back(name);
    });
    CHECK(global_names.size() == 1);
    CHECK(global_names[0] == String("global_count"));

    // Verify functions enumerable with signatures
    Vector<String> func_names;
    index.for_each_function([&](const String& name) {
        func_names.push_back(name);
    });
    CHECK(func_names.size() == 1);
    CHECK(func_names[0] == String("helper"));
    CHECK(index.find_function_signature("helper") == StringView("(): i32"));
}
