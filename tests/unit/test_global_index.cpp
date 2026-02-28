#include "roxy/core/doctest/doctest.h"

#include "roxy/lsp/global_index.hpp"
#include "roxy/lsp/lsp_parser.hpp"
#include "roxy/lsp/indexer.hpp"
#include "roxy/lsp/protocol.hpp"
#include "roxy/core/bump_allocator.hpp"

#include <cstring>

using namespace rx;

// Helper to parse and index source
static FileStubs parse_and_index(const char* source, BumpAllocator& allocator) {
    u32 length = static_cast<u32>(strlen(source));
    Lexer lexer(source, length);
    LspParser parser(lexer, allocator);
    SyntaxTree tree = parser.parse();

    FileIndexer indexer;
    return indexer.index(tree.root);
}

// --- GlobalIndex unit tests ---

TEST_CASE("GlobalIndex: Empty index returns nullptr") {
    GlobalIndex index;
    CHECK(index.find_struct("Foo") == nullptr);
    CHECK(index.find_enum("Bar") == nullptr);
    CHECK(index.find_function("baz") == nullptr);
    CHECK(index.find_trait("Qux") == nullptr);
    CHECK(index.find_global("g") == nullptr);
    CHECK(index.find_method("Foo", "bar") == nullptr);
    CHECK(index.find_constructor("Foo", "") == nullptr);
    CHECK(index.find_field("Foo", "x") == nullptr);
    CHECK(index.find_any("Foo").empty());
}

TEST_CASE("GlobalIndex: Single file with struct") {
    BumpAllocator allocator(4096);
    const char* source = "struct Point { x: f32; y: f32; }";
    FileStubs stubs = parse_and_index(source, allocator);

    GlobalIndex index;
    String uri("file:///test.roxy");
    index.update_file(uri, stubs);

    const SymbolLocation* loc = index.find_struct("Point");
    REQUIRE(loc != nullptr);
    CHECK(StringView(loc->uri.data(), loc->uri.size()) == StringView("file:///test.roxy"));
    CHECK(loc->range.start == 0);
    // name_range should cover "Point" (at offset 7, length 5)
    CHECK(loc->name_range.start == 7);
    CHECK(loc->name_range.end == 12);
}

TEST_CASE("GlobalIndex: Single file with function") {
    BumpAllocator allocator(4096);
    const char* source = "fun add(a: i32, b: i32): i32 { return a + b; }";
    FileStubs stubs = parse_and_index(source, allocator);

    GlobalIndex index;
    String uri("file:///test.roxy");
    index.update_file(uri, stubs);

    const SymbolLocation* loc = index.find_function("add");
    REQUIRE(loc != nullptr);
    CHECK(loc->name_range.start == 4);
    CHECK(loc->name_range.end == 7);
}

TEST_CASE("GlobalIndex: Multiple files") {
    BumpAllocator allocator1(4096);
    const char* source1 = "struct Point { x: f32; y: f32; }";
    FileStubs stubs1 = parse_and_index(source1, allocator1);

    BumpAllocator allocator2(4096);
    const char* source2 = "fun add(a: i32, b: i32): i32 { return a + b; }";
    FileStubs stubs2 = parse_and_index(source2, allocator2);

    GlobalIndex index;
    String uri1("file:///a.roxy");
    String uri2("file:///b.roxy");
    index.update_file(uri1, stubs1);
    index.update_file(uri2, stubs2);

    CHECK(index.find_struct("Point") != nullptr);
    CHECK(index.find_function("add") != nullptr);
}

TEST_CASE("GlobalIndex: update_file replaces old entries") {
    BumpAllocator allocator1(4096);
    const char* source1 = "struct Foo { x: i32; }";
    FileStubs stubs1 = parse_and_index(source1, allocator1);

    GlobalIndex index;
    String uri("file:///test.roxy");
    index.update_file(uri, stubs1);

    const SymbolLocation* loc1 = index.find_struct("Foo");
    REQUIRE(loc1 != nullptr);

    // Update with different content
    BumpAllocator allocator2(4096);
    const char* source2 = "struct Bar { y: f64; }";
    FileStubs stubs2 = parse_and_index(source2, allocator2);
    index.update_file(uri, stubs2);

    CHECK(index.find_struct("Foo") == nullptr);
    CHECK(index.find_struct("Bar") != nullptr);
}

TEST_CASE("GlobalIndex: remove_file clears entries") {
    BumpAllocator allocator(4096);
    const char* source = "struct Point { x: f32; }";
    FileStubs stubs = parse_and_index(source, allocator);

    GlobalIndex index;
    String uri("file:///test.roxy");
    index.update_file(uri, stubs);

    REQUIRE(index.find_struct("Point") != nullptr);

    index.remove_file(StringView("file:///test.roxy"));
    CHECK(index.find_struct("Point") == nullptr);
}

TEST_CASE("GlobalIndex: find_method") {
    BumpAllocator allocator(4096);
    const char* source = "fun Point.sum(): f32 { return 0.0; }";
    FileStubs stubs = parse_and_index(source, allocator);

    GlobalIndex index;
    String uri("file:///test.roxy");
    index.update_file(uri, stubs);

    const SymbolLocation* loc = index.find_method("Point", "sum");
    REQUIRE(loc != nullptr);
    CHECK(StringView(loc->uri.data(), loc->uri.size()) == StringView("file:///test.roxy"));
}

TEST_CASE("GlobalIndex: find_field") {
    BumpAllocator allocator(4096);
    const char* source = "struct Point { x: f32; y: f32; }";
    FileStubs stubs = parse_and_index(source, allocator);

    GlobalIndex index;
    String uri("file:///test.roxy");
    index.update_file(uri, stubs);

    const SymbolLocation* loc = index.find_field("Point", "x");
    REQUIRE(loc != nullptr);

    const SymbolLocation* loc2 = index.find_field("Point", "y");
    REQUIRE(loc2 != nullptr);

    CHECK(index.find_field("Point", "z") == nullptr);
}

TEST_CASE("GlobalIndex: find_constructor") {
    BumpAllocator allocator(4096);
    const char* source = "fun new Point(x: f32, y: f32) { }";
    FileStubs stubs = parse_and_index(source, allocator);

    GlobalIndex index;
    String uri("file:///test.roxy");
    index.update_file(uri, stubs);

    // Default constructor: key = "Point."
    const SymbolLocation* loc = index.find_constructor("Point", "");
    REQUIRE(loc != nullptr);
}

TEST_CASE("GlobalIndex: find_any returns struct match") {
    BumpAllocator allocator(4096);
    const char* source = "struct Point { x: f32; }";
    FileStubs stubs = parse_and_index(source, allocator);

    GlobalIndex index;
    String uri("file:///test.roxy");
    index.update_file(uri, stubs);

    Vector<SymbolLocation> results = index.find_any("Point");
    CHECK(results.size() == 1);
}

TEST_CASE("GlobalIndex: find_any with multiple categories") {
    // Create a scenario where a name appears in multiple categories
    // (unlikely in practice, but tests the multi-category search)
    BumpAllocator allocator1(4096);
    const char* source1 = "struct Foo { x: i32; }";
    FileStubs stubs1 = parse_and_index(source1, allocator1);

    BumpAllocator allocator2(4096);
    const char* source2 = "fun Foo(): i32 { return 0; }";
    FileStubs stubs2 = parse_and_index(source2, allocator2);

    GlobalIndex index;
    String uri1("file:///a.roxy");
    String uri2("file:///b.roxy");
    index.update_file(uri1, stubs1);
    index.update_file(uri2, stubs2);

    Vector<SymbolLocation> results = index.find_any("Foo");
    CHECK(results.size() == 2);
}

TEST_CASE("GlobalIndex: Enum indexing") {
    BumpAllocator allocator(4096);
    const char* source = "enum Color { Red, Green, Blue }";
    FileStubs stubs = parse_and_index(source, allocator);

    GlobalIndex index;
    String uri("file:///test.roxy");
    index.update_file(uri, stubs);

    const SymbolLocation* loc = index.find_enum("Color");
    REQUIRE(loc != nullptr);
    CHECK(loc->name_range.start == 5);
    CHECK(loc->name_range.end == 10);
}

TEST_CASE("GlobalIndex: Trait indexing") {
    BumpAllocator allocator(4096);
    const char* source = "trait Printable;";
    FileStubs stubs = parse_and_index(source, allocator);

    GlobalIndex index;
    String uri("file:///test.roxy");
    index.update_file(uri, stubs);

    const SymbolLocation* loc = index.find_trait("Printable");
    REQUIRE(loc != nullptr);
}

TEST_CASE("GlobalIndex: Global variable indexing") {
    BumpAllocator allocator(4096);
    const char* source = "var pi: f64 = 3.14;";
    FileStubs stubs = parse_and_index(source, allocator);

    GlobalIndex index;
    String uri("file:///test.roxy");
    index.update_file(uri, stubs);

    const SymbolLocation* loc = index.find_global("pi");
    REQUIRE(loc != nullptr);
}

// --- Integration tests: find_node_at_offset ---

TEST_CASE("GlobalIndex: find_node_at_offset on type reference") {
    BumpAllocator allocator(4096);
    const char* source = "var x: Point;";
    u32 length = static_cast<u32>(strlen(source));
    Lexer lexer(source, length);
    LspParser parser(lexer, allocator);
    SyntaxTree tree = parser.parse();

    // "Point" starts at offset 7
    SyntaxNode* node = find_node_at_offset(tree.root, 7);
    REQUIRE(node != nullptr);
    CHECK(node->kind == SyntaxKind::TokenIdentifier);
    CHECK(node->token.text() == StringView("Point"));
}

TEST_CASE("GlobalIndex: find_node_at_offset on function call") {
    BumpAllocator allocator(4096);
    const char* source = "fun main() { add(1, 2); }";
    u32 length = static_cast<u32>(strlen(source));
    Lexer lexer(source, length);
    LspParser parser(lexer, allocator);
    SyntaxTree tree = parser.parse();

    // "add" starts at offset 14
    SyntaxNode* node = find_node_at_offset(tree.root, 14);
    REQUIRE(node != nullptr);
    CHECK(node->kind == SyntaxKind::TokenIdentifier);
    CHECK(node->token.text() == StringView("add"));
}

// --- Integration tests: lsp_position_to_offset ---

TEST_CASE("GlobalIndex: lsp_position_to_offset basic") {
    const char* source = "line one\nline two\nline three";
    u32 length = static_cast<u32>(strlen(source));

    // Line 0, char 0 -> offset 0
    CHECK(lsp_position_to_offset(source, length, LspPosition{0, 0}) == 0);

    // Line 0, char 5 -> offset 5
    CHECK(lsp_position_to_offset(source, length, LspPosition{0, 5}) == 5);

    // Line 1, char 0 -> offset 9 (after "line one\n")
    CHECK(lsp_position_to_offset(source, length, LspPosition{1, 0}) == 9);

    // Line 1, char 4 -> offset 13
    CHECK(lsp_position_to_offset(source, length, LspPosition{1, 4}) == 13);

    // Line 2, char 0 -> offset 18 (after "line one\nline two\n")
    CHECK(lsp_position_to_offset(source, length, LspPosition{2, 0}) == 18);
}

TEST_CASE("GlobalIndex: lsp_position_to_offset clamped") {
    const char* source = "short";
    u32 length = static_cast<u32>(strlen(source));

    // Past end of line
    CHECK(lsp_position_to_offset(source, length, LspPosition{0, 100}) == length);

    // Past end of file (line doesn't exist)
    CHECK(lsp_position_to_offset(source, length, LspPosition{5, 0}) == length);
}

// --- Integration: parse + index + lookup round-trip ---

TEST_CASE("GlobalIndex: Parse struct, index, lookup name_range matches CST") {
    BumpAllocator allocator(4096);
    const char* source = "struct Vec2 { x: f32; y: f32; }";
    FileStubs stubs = parse_and_index(source, allocator);

    GlobalIndex index;
    String uri("file:///test.roxy");
    index.update_file(uri, stubs);

    const SymbolLocation* loc = index.find_struct("Vec2");
    REQUIRE(loc != nullptr);

    // Verify the name_range covers exactly "Vec2" in the source
    CHECK(loc->name_range.end - loc->name_range.start == 4);
    CHECK(memcmp(source + loc->name_range.start, "Vec2", 4) == 0);
}

TEST_CASE("GlobalIndex: Parse method, index, lookup range matches") {
    BumpAllocator allocator(4096);
    const char* source = "fun Vec2.length(): f32 { return 0.0; }";
    FileStubs stubs = parse_and_index(source, allocator);

    GlobalIndex index;
    String uri("file:///test.roxy");
    index.update_file(uri, stubs);

    const SymbolLocation* loc = index.find_method("Vec2", "length");
    REQUIRE(loc != nullptr);

    // Verify name_range covers "length"
    u32 name_len = loc->name_range.end - loc->name_range.start;
    CHECK(name_len == 6);
    CHECK(memcmp(source + loc->name_range.start, "length", 6) == 0);
}

TEST_CASE("GlobalIndex: Named constructor lookup") {
    BumpAllocator allocator(4096);
    const char* source = "fun new Point.origin() { }";
    FileStubs stubs = parse_and_index(source, allocator);

    GlobalIndex index;
    String uri("file:///test.roxy");
    index.update_file(uri, stubs);

    const SymbolLocation* loc = index.find_constructor("Point", "origin");
    REQUIRE(loc != nullptr);

    // Verify name_range covers "origin"
    u32 name_len = loc->name_range.end - loc->name_range.start;
    CHECK(name_len == 6);
    CHECK(memcmp(source + loc->name_range.start, "origin", 6) == 0);
}
